/*
 * gen_trainer_family — R7A trainer-front family generator (extended for the
 * R8 trainer-back family).
 *
 * Consumes the SINGLE declarative family descriptor
 * (resources/extraction/emerald/bpee01/trainer_front_family.toml) and emits
 * four deterministic, bytewise-sorted metadata files:
 *
 *   catalog.toml                       — full family resource catalog (186).
 *   bindings.generated.toml            — semantic extraction bindings (186).
 *   ownership.generated.toml           — native/GBA ownership per resource (186).
 *   trainer_front_consumers.generated.toml — machine-readable consumer map.
 *
 * With --back-descriptor
 * (resources/extraction/emerald/bpee01/trainer_back_family.toml) the same run
 * also emits the R8 back-family bindings/ownership/consumers files and the
 * catalog becomes the union (196 = 186 front + 10 back: 8 raw back sheets,
 * 2 unique back palettes; the 6 alias back palettes are the R7B front normal
 * palettes and are NOT duplicated). The front-family outputs are byte-identical
 * with or without --back-descriptor.
 *
 * The generator is game-neutral: every Emerald-specific fact (trainer list,
 * artifact names, ID templates, ownership states, symbol/artifact templates,
 * consumer table names) lives in the descriptor; this file contains no game
 * data beyond the per-table consumer-site inventory (a fixed, audited list of
 * GBA source locations; see kTableSites* below).
 *
 * Determinism (§17): output order is independent of descriptor order. All
 * resources are sorted bytewise by canonical id; all consumer rows are sorted
 * deterministically. Running the generator twice — even with the [[trainers]]
 * rows shuffled — produces byte-identical files.
 *
 * Fail-closed validation (§18): every check below reports a structured
 * diagnostic ("FAIL: <message>") and returns a nonzero exit code.
 *
 * Usage:
 *   gen_trainer_family --descriptor PATH \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH
 *   gen_trainer_family --descriptor PATH --back-descriptor PATH \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH \
 *       --back-bindings-out PATH --back-ownership-out PATH \
 *       --back-consumers-out PATH
 *   gen_trainer_family --descriptor PATH [--back-descriptor PATH] --check \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH \
 *       [--back-bindings-out PATH --back-ownership-out PATH \
 *        --back-consumers-out PATH]
 *
 *   --check verifies the files at the given paths are byte-identical to what
 *   would be generated (idempotency gate) without writing anything.
 *
 * Build: links the gen3 core (toml.c, sha256.c, lz77.c, resource_id.c, util.c).
 */

#include "gen3/resources/lz77.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Limits and constants                                                */
/* ------------------------------------------------------------------ */

#define MAX_TRAINERS 256u
#define MAX_RESOURCES (MAX_TRAINERS * 2u)
#define MAX_TABLE_SITES 64u
#define MAX_SLOTS_PER_RESOURCE 2u
#define ARTIFACT_BUF 512u

/* The tables the consumers map records, in fixed emission order. A family
 * descriptor names the tables it uses (sheet_table / palette_table); the
 * consumers file emits sites only for those tables, so the front family (3
 * tables) and the back family (gTrainerBackPicTable + back palette table) each
 * get exactly their own rows. */
static const char *const kKnownTables[] =
{
    "gTrainerFrontPicTable",
    "gTrainerFrontPicPaletteTable",
    "gTrainerBackPicPaletteTable",
    "gTrainerBackPicTable",
};
#define KNOWN_TABLE_COUNT (sizeof(kKnownTables) / sizeof(kKnownTables[0]))

/* Consumer-site inventory (GBA build): the checked-in source locations that
 * read each table generically by trainer index / pic id, sorted bytewise for
 * deterministic output. Fixed audit artifact (docs/
 * R7A_TRAINER_FRONT_FAMILY_AUDIT.md §6); the machine-readable output below is
 * derived from it, so this list is the single place the sites live. */
static const char *const kTableSitesFrontPicTable[] =
{
    "src/battle_gfx_sfx_util.c:704",
    "src/field_effect.c:897",
    "src/field_effect.c:898",
    "src/field_effect.c:910",
    "src/pokenav_match_call_gfx.c:1250",
    "src/trainer_pokemon_sprites.c:82",
};

static const char *const kTableSitesFrontPaletteTable[] =
{
    "src/battle_controller_link_opponent.c:1297",
    "src/battle_controller_link_opponent.c:1320",
    "src/battle_controller_opponent.c:1318",
    "src/battle_controller_opponent.c:1388",
    "src/battle_controller_player.c:2329",
    "src/battle_controller_player_partner.c:1332",
    "src/battle_controller_player_partner.c:1802",
    "src/battle_controller_recorded_opponent.c:1247",
    "src/battle_controller_recorded_player.c:1232",
    "src/battle_gfx_sfx_util.c:707",
    "src/battle_gfx_sfx_util.c:726",
    "src/field_effect.c:896",
    "src/field_effect.c:899",
    "src/field_effect.c:911",
    "src/pokenav_match_call_gfx.c:1251",
    "src/trainer_pokemon_sprites.c:114",
    "src/trainer_pokemon_sprites.c:119",
    "src/trainer_pokemon_sprites.c:129",
};

static const char *const kTableSitesBackPaletteTable[] =
{
    "src/battle_controller_link_partner.c:1561",
    "src/battle_controller_player.c:2963",
    "src/battle_controller_player_partner.c:1797",
    "src/battle_controller_recorded_player.c:1681",
    "src/battle_controller_wally.c:1448",
    "src/battle_gfx_sfx_util.c:716",
};

/* R8: gTrainerBackPicTable (CompressedSpriteSheet) read sites. 713 is the
 * live DecompressTrainerBackPic path (the LZ77-of-raw output is dead scratch
 * but the .data pointer must stay non-NULL); 84 is the dead
 * CreateTrainerCardSprite path (no callers). Both are checked-in read sites,
 * listed like the R7A front-table dead site (trainer_pokemon_sprites.c:82). */
static const char *const kTableSitesBackPicTable[] =
{
    "src/battle_gfx_sfx_util.c:713",
    "src/trainer_pokemon_sprites.c:84",
};

struct SiteList
{
    const char *const *sites;
    size_t count;
};

static struct SiteList SiteListForTable(const char *tableName)
{
    struct SiteList list = { NULL, 0u };
    if (strcmp(tableName, "gTrainerFrontPicTable") == 0)
    {
        list.sites = kTableSitesFrontPicTable;
        list.count = sizeof(kTableSitesFrontPicTable) / sizeof(kTableSitesFrontPicTable[0]);
    }
    else if (strcmp(tableName, "gTrainerFrontPicPaletteTable") == 0)
    {
        list.sites = kTableSitesFrontPaletteTable;
        list.count = sizeof(kTableSitesFrontPaletteTable) / sizeof(kTableSitesFrontPaletteTable[0]);
    }
    else if (strcmp(tableName, "gTrainerBackPicPaletteTable") == 0)
    {
        list.sites = kTableSitesBackPaletteTable;
        list.count = sizeof(kTableSitesBackPaletteTable) / sizeof(kTableSitesBackPaletteTable[0]);
    }
    else if (strcmp(tableName, "gTrainerBackPicTable") == 0)
    {
        list.sites = kTableSitesBackPicTable;
        list.count = sizeof(kTableSitesBackPicTable) / sizeof(kTableSitesBackPicTable[0]);
    }
    return list;
}

/* ------------------------------------------------------------------ */
/* Family model                                                        */
/* ------------------------------------------------------------------ */

enum PaletteDir
{
    PALETTE_DIR_FRONT_PICS,
    PALETTE_DIR_PALETTES,
    PALETTE_DIR_BACK_PICS,      /* R8: graphics/trainers/back_pics dir */
};

enum ResourceKind
{
    RESOURCE_SHEET,
    RESOURCE_PALETTE,
};

struct FamilyTrainer
{
    char canonical[128];
    char slug[128];
    char symbol[128];           /* suffix, e.g. "AquaGruntM" */
    char pic[128];              /* TRAINER_PIC_ constant suffix */
    long long index;
    long long sizeMult;
    enum PaletteDir paletteDir;
    char ownershipNative[40];
    long long backPaletteIndex; /* -1 when none */
    /* R8 back family: per-trainer overrides. sheetSize 0 = family default.
     * frames > 0 declares a SpriteFrameImage frame array (frame_array) that
     * consumes this sheet resource at frames 0..frames-1. */
    long long sheetSize;
    long long frames;
    char frameArray[192];
    bool hasPalette;            /* front: always true; back: red/leaf only */
};

struct Resource
{
    enum ResourceKind kind;
    char id[GEN3_RESOURCE_NAME_MAX + 1u];
    char type[32];
    long long schema;
    bool requiredForBase;
    char symbol[192];           /* full legacy symbol, e.g. "gTrainerFrontPic_AquaGruntM" */
    char sourceArtifact[ARTIFACT_BUF];
    char sourceEncoding[32];
    char canonicalRepresentation[32];
    long long expectedDecodedSize;
    size_t encodedLength;
    size_t decodedLength;
    uint8_t encodedSha[GEN3_RESOURCE_KEY_SIZE];
    uint8_t decodedSha[GEN3_RESOURCE_KEY_SIZE];
    char ownershipState[40];
    char nativeTarget[40];
    /* consumer table slots: primary table + optional back slot. */
    struct Slot
    {
        char table[64];
        long long index;
    } slots[MAX_SLOTS_PER_RESOURCE];
    size_t slotCount;
    /* R8: SpriteFrameImage frame-array consumer (emitted in the frame_slots
     * section); frames 0 when the resource has no frame array. */
    long long frames;
    char frameArray[192];
};

struct Family
{
    char game[64];
    char romProfile[64];
    char namespace[64];
    char resourceApi[32];
    long long catalogVersion;
    char sheetIdTemplate[256];
    char paletteIdTemplate[256];
    char sheetType[32];
    char paletteType[32];
    long long schema;
    bool requiredForBase;
    long long sheetDecodedSize;
    long long paletteDecodedSize;
    char sheetEncoding[32];
    char paletteEncoding[32];
    char sheetRepresentation[32];
    char paletteRepresentation[32];
    /* R8: family shape. The front family binds "gTrainerFrontPic_<s>"/"gTrainer-
     * Palette_<s>" symbols into gTrainerFrontPicTable/gTrainerFrontPicPalette-
     * Table from graphics/trainers/front_pics; the back family binds "gTrainer-
     * BackPic_<s>"/"gTrainerBackPicPalette_<s>" into gTrainerBackPicTable/
     * gTrainerBackPicPaletteTable from graphics/trainers/back_pics. All
     * template values live in the descriptor; nothing here is family-specific. */
    char sheetSymbolTemplate[192];
    char paletteSymbolTemplate[192];
    char sheetArtifactTemplate[ARTIFACT_BUF];
    char sheetTable[64];
    char paletteTable[64];
    char familyLabel[32];       /* "front" or "back": comment wording only */

    size_t trainerCount;
    struct FamilyTrainer trainers[MAX_TRAINERS];

    size_t resourceCount;
    struct Resource resources[MAX_RESOURCES];
};

/* ------------------------------------------------------------------ */
/* Error reporting                                                     */
/* ------------------------------------------------------------------ */

static int gExitCode = 0;

static void Fail(const char *format, ...)
{
    va_list args;
    if (gExitCode == 0)
        gExitCode = 1;
    fputs("FAIL: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* Descriptor parsing                                                  */
/* ------------------------------------------------------------------ */

static bool GetStringRequired(const struct Gen3TomlMap *map, const char *key,
                              char *out, size_t outSize, const char *what)
{
    const char *value = NULL;
    if (!Gen3Toml_GetString(map, key, &value))
    {
        Fail("family descriptor missing %s ('%s')", what, key);
        return false;
    }
    if (strlen(value) >= outSize)
    {
        Fail("family descriptor field '%s' is too long", key);
        return false;
    }
    strcpy(out, value);
    return true;
}

static bool GetIntegerRequired(const struct Gen3TomlMap *map, const char *key,
                               long long *out, const char *what)
{
    if (!Gen3Toml_GetInteger(map, key, out))
    {
        Fail("family descriptor missing %s ('%s')", what, key);
        return false;
    }
    return true;
}

static bool GetBoolRequired(const struct Gen3TomlMap *map, const char *key,
                            bool *out, const char *what)
{
    if (!Gen3Toml_GetBool(map, key, out))
    {
        Fail("family descriptor missing %s ('%s')", what, key);
        return false;
    }
    return true;
}

static bool ParseFamily(const char *descriptorPath, struct Family *family)
{
    struct Gen3Buffer text;
    struct Gen3TomlDocument doc;
    size_t trainerCount;
    size_t i;
    char errbuf[512];

    memset(family, 0, sizeof(*family));

    /* Gen3Util_ReadFile owns buffer init; a caller-side Gen3Buffer_Init would
     * leak its allocation (caught by the ASan/UBSan runner). */
    if (!Gen3Util_ReadFile(descriptorPath, &text, errbuf, sizeof(errbuf)))
    {
        Fail("cannot read descriptor: %s", errbuf);
        Gen3Buffer_Destroy(&text);
        return false;
    }
    if (!Gen3Toml_Parse(text.data, text.length, &doc, errbuf, sizeof(errbuf)))
    {
        Fail("malformed family descriptor: %s", errbuf);
        Gen3Buffer_Destroy(&text);
        return false;
    }

    if (!GetStringRequired(&doc.root, "game", family->game, sizeof(family->game), "game")
     || !GetStringRequired(&doc.root, "rom_profile", family->romProfile, sizeof(family->romProfile), "rom_profile")
     || !GetStringRequired(&doc.root, "namespace", family->namespace, sizeof(family->namespace), "namespace")
     || !GetStringRequired(&doc.root, "resource_api", family->resourceApi, sizeof(family->resourceApi), "resource_api")
     || !GetIntegerRequired(&doc.root, "catalog_version", &family->catalogVersion, "catalog_version")
     || !GetStringRequired(&doc.root, "sheet_id_template", family->sheetIdTemplate, sizeof(family->sheetIdTemplate), "sheet_id_template")
     || !GetStringRequired(&doc.root, "palette_id_template", family->paletteIdTemplate, sizeof(family->paletteIdTemplate), "palette_id_template")
     || !GetStringRequired(&doc.root, "resource_type_sheet", family->sheetType, sizeof(family->sheetType), "resource_type_sheet")
     || !GetStringRequired(&doc.root, "resource_type_palette", family->paletteType, sizeof(family->paletteType), "resource_type_palette")
     || !GetIntegerRequired(&doc.root, "schema", &family->schema, "schema")
     || !GetBoolRequired(&doc.root, "required_for_base", &family->requiredForBase, "required_for_base")
     || !GetIntegerRequired(&doc.root, "sheet_decoded_size", &family->sheetDecodedSize, "sheet_decoded_size")
     || !GetIntegerRequired(&doc.root, "palette_decoded_size", &family->paletteDecodedSize, "palette_decoded_size")
     || !GetStringRequired(&doc.root, "sheet_encoding", family->sheetEncoding, sizeof(family->sheetEncoding), "sheet_encoding")
     || !GetStringRequired(&doc.root, "palette_encoding", family->paletteEncoding, sizeof(family->paletteEncoding), "palette_encoding")
     || !GetStringRequired(&doc.root, "sheet_representation", family->sheetRepresentation, sizeof(family->sheetRepresentation), "sheet_representation")
     || !GetStringRequired(&doc.root, "palette_representation", family->paletteRepresentation, sizeof(family->paletteRepresentation), "palette_representation")
     || !GetStringRequired(&doc.root, "sheet_symbol_template", family->sheetSymbolTemplate, sizeof(family->sheetSymbolTemplate), "sheet_symbol_template")
     || !GetStringRequired(&doc.root, "palette_symbol_template", family->paletteSymbolTemplate, sizeof(family->paletteSymbolTemplate), "palette_symbol_template")
     || !GetStringRequired(&doc.root, "sheet_artifact_template", family->sheetArtifactTemplate, sizeof(family->sheetArtifactTemplate), "sheet_artifact_template")
     || !GetStringRequired(&doc.root, "sheet_table", family->sheetTable, sizeof(family->sheetTable), "sheet_table")
     || !GetStringRequired(&doc.root, "palette_table", family->paletteTable, sizeof(family->paletteTable), "palette_table")
     || !GetStringRequired(&doc.root, "family_label", family->familyLabel, sizeof(family->familyLabel), "family_label"))
    {
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }
    if (strcmp(family->familyLabel, "front") != 0
     && strcmp(family->familyLabel, "back") != 0)
    {
        Fail("unsupported family_label '%s' (front or back)", family->familyLabel);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }

    /* Unsupported encodings fail closed (§18). Sheets may be gba-lz77 (front
     * family) or raw (R8 back family: the .4bpp files are already decoded);
     * palettes are always gba-lz77 in this game. */
    if (strcmp(family->sheetEncoding, "gba-lz77") != 0
     && strcmp(family->sheetEncoding, "raw") != 0)
    {
        Fail("unsupported sheet source encoding '%s' (gba-lz77 or raw)", family->sheetEncoding);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }
    if (strcmp(family->paletteEncoding, "gba-lz77") != 0)
    {
        Fail("unsupported palette source encoding '%s' (gba-lz77 only)", family->paletteEncoding);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }

    /* The named consumer tables must be known to the site inventory (fail
     * closed: an unknown table would silently emit no consumer sites). */
    if (SiteListForTable(family->sheetTable).count == 0u
     || SiteListForTable(family->paletteTable).count == 0u)
    {
        Fail("unknown consumer table '%s'/'%s' (not in the site inventory)",
             family->sheetTable, family->paletteTable);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }

    trainerCount = Gen3Toml_GetArrayCount(&doc.root, "trainers");
    if (trainerCount == 0u)
    {
        Fail("family descriptor has no [[trainers]] entries");
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }
    if (trainerCount > MAX_TRAINERS)
    {
        Fail("family descriptor has %zu trainers, limit is %u", trainerCount, MAX_TRAINERS);
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return false;
    }

    for (i = 0u; i < trainerCount; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&doc.root, "trainers", i);
        struct FamilyTrainer *t = &family->trainers[family->trainerCount];
        char paletteDir[32];
        long long backIndex = -1;

        if (item == NULL
         || !GetStringRequired(item, "canonical", t->canonical, sizeof(t->canonical), "canonical")
         || !GetStringRequired(item, "slug", t->slug, sizeof(t->slug), "slug")
         || !GetStringRequired(item, "symbol", t->symbol, sizeof(t->symbol), "symbol")
         || !GetStringRequired(item, "pic", t->pic, sizeof(t->pic), "pic")
         || !GetIntegerRequired(item, "index", &t->index, "index")
         || !GetIntegerRequired(item, "size_mult", &t->sizeMult, "size_mult")
         || !GetStringRequired(item, "palette_dir", paletteDir, sizeof(paletteDir), "palette_dir")
         || !GetStringRequired(item, "ownership_native", t->ownershipNative, sizeof(t->ownershipNative), "ownership_native"))
        {
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&text);
            return false;
        }
        if (strcmp(paletteDir, "front_pics") == 0)
            t->paletteDir = PALETTE_DIR_FRONT_PICS;
        else if (strcmp(paletteDir, "palettes") == 0)
            t->paletteDir = PALETTE_DIR_PALETTES;
        else if (strcmp(paletteDir, "back_pics") == 0)
            t->paletteDir = PALETTE_DIR_BACK_PICS;
        else
        {
            Fail("unsupported palette_dir '%s' for '%s'", paletteDir, t->canonical);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&text);
            return false;
        }

        if (Gen3Toml_GetInteger(item, "back_palette_index", &backIndex))
            t->backPaletteIndex = backIndex;
        else
            t->backPaletteIndex = -1;

        /* R8 optional per-trainer overrides. */
        {
            const char *frameArray = NULL;
            t->hasPalette = true;
            if (Gen3Toml_GetInteger(item, "sheet_size", &t->sheetSize) && t->sheetSize <= 0)
            {
                Fail("sheet_size must be positive for '%s'", t->canonical);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&text);
                return false;
            }
            if (Gen3Toml_GetInteger(item, "frames", &t->frames) && t->frames < 0)
            {
                Fail("frames must be non-negative for '%s'", t->canonical);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&text);
                return false;
            }
            if (Gen3Toml_GetString(item, "frame_array", &frameArray))
            {
                if (strlen(frameArray) >= sizeof(t->frameArray))
                {
                    Fail("frame_array too long for '%s'", t->canonical);
                    Gen3Toml_Destroy(&doc);
                    Gen3Buffer_Destroy(&text);
                    return false;
                }
                strcpy(t->frameArray, frameArray);
            }
            if (t->frames > 0 && t->frameArray[0] == '\0')
            {
                Fail("frames > 0 but no frame_array given for '%s'", t->canonical);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&text);
                return false;
            }
            if (t->frames == 0 && t->frameArray[0] != '\0')
            {
                Fail("frame_array without frames for '%s'", t->canonical);
                Gen3Toml_Destroy(&doc);
                Gen3Buffer_Destroy(&text);
                return false;
            }
            if (Gen3Toml_GetBool(item, "has_palette", &t->hasPalette) && !t->hasPalette)
                t->hasPalette = false;
        }

        family->trainerCount++;
    }

    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&text);
    return true;
}

/* ------------------------------------------------------------------ */
/* Resource model construction                                         */
/* ------------------------------------------------------------------ */

static bool BuildId(char *out, size_t outSize, const char *template,
                    const char *canonical)
{
    int n = snprintf(out, outSize, template, canonical);
    if (n < 0 || (size_t)n >= outSize)
    {
        Fail("canonical id exceeds buffer (%s)", canonical);
        return false;
    }
    return true;
}

static bool DecodeArtifact(const char *path, long long expectedSize,
                           const char *encoding,
                           uint8_t encodedSha[GEN3_RESOURCE_KEY_SIZE],
                           uint8_t decodedSha[GEN3_RESOURCE_KEY_SIZE],
                           size_t *outEncodedLength, size_t *outDecodedLength)
{
    struct Gen3Buffer encoded;
    struct Gen3Buffer decoded;
    struct Gen3Sha256Context ctx;
    size_t decodedSize = 0u;
    char errbuf[512];
    enum Gen3Lz77Result result;

    /* ReadFile owns `encoded` init (see ParseFamily note). */
    if (!Gen3Util_ReadFile(path, &encoded, errbuf, sizeof(errbuf)))
    {
        Fail("source artifact missing: %s (%s)", path, errbuf);
        Gen3Buffer_Destroy(&encoded);
        return false;
    }
    if (expectedSize < 0 || (size_t)expectedSize > SIZE_MAX - 1u)
    {
        Fail("invalid expected decoded size %lld", expectedSize);
        Gen3Buffer_Destroy(&encoded);
        return false;
    }

    if (strcmp(encoding, "raw") == 0)
    {
        /* R8 back sheets: the .4bpp artifact IS the canonical decoded payload
         * (uncompressed 4bpp tiles). encoded == decoded, one hash. */
        if ((long long)encoded.length != expectedSize)
        {
            Fail("raw artifact size conflict for %s: %zu bytes, "
                 "family expects %lld", path, encoded.length, expectedSize);
            Gen3Buffer_Destroy(&encoded);
            return false;
        }
        Gen3Sha256_Init(&ctx);
        Gen3Sha256_Update(&ctx, encoded.data, encoded.length);
        Gen3Sha256_Final(&ctx, encodedSha);
        memcpy(decodedSha, encodedSha, GEN3_RESOURCE_KEY_SIZE);
        *outEncodedLength = encoded.length;
        *outDecodedLength = encoded.length;
        Gen3Buffer_Destroy(&encoded);
        return true;
    }

    Gen3Buffer_Init(&decoded, (size_t)expectedSize);
    result = Gen3Lz77_Decode((const uint8_t *)encoded.data, encoded.length,
                             (uint8_t *)decoded.data, (size_t)expectedSize, &decodedSize);
    if (result != GEN3_LZ77_OK)
    {
        Fail("LZ77 decode failed for %s (result %d)", path, (int)result);
        Gen3Buffer_Destroy(&encoded);
        Gen3Buffer_Destroy(&decoded);
        return false;
    }
    if ((long long)decodedSize != expectedSize)
    {
        Fail("decoded size conflict for %s: stream declares %zu bytes, "
             "family expects %lld", path, decodedSize, expectedSize);
        Gen3Buffer_Destroy(&encoded);
        Gen3Buffer_Destroy(&decoded);
        return false;
    }

    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, encoded.data, encoded.length);
    Gen3Sha256_Final(&ctx, encodedSha);

    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, (uint8_t *)decoded.data, decodedSize);
    Gen3Sha256_Final(&ctx, decodedSha);

    *outEncodedLength = encoded.length;
    *outDecodedLength = decodedSize;

    Gen3Buffer_Destroy(&encoded);
    Gen3Buffer_Destroy(&decoded);
    return true;
}

static bool BuildResources(struct Family *family)
{
    size_t i;
    for (i = 0u; i < family->trainerCount; i++)
    {
        const struct FamilyTrainer *t = &family->trainers[i];
        struct Resource *sheet = &family->resources[family->resourceCount++];
        struct Resource *palette = NULL;
        long long sheetSize;
        char artifact[ARTIFACT_BUF];

        memset(sheet, 0, sizeof(*sheet));
        if (t->hasPalette)
            palette = &family->resources[family->resourceCount++];

        if (!BuildId(sheet->id, sizeof(sheet->id), family->sheetIdTemplate, t->canonical))
            return false;
        if (palette != NULL
         && !BuildId(palette->id, sizeof(palette->id), family->paletteIdTemplate, t->canonical))
            return false;

        sheet->kind = RESOURCE_SHEET;
        strcpy(sheet->type, family->sheetType);
        sheet->schema = family->schema;
        sheet->requiredForBase = family->requiredForBase;
        if (palette != NULL)
        {
            palette->kind = RESOURCE_PALETTE;
            strcpy(palette->type, family->paletteType);
            palette->schema = family->schema;
            palette->requiredForBase = family->requiredForBase;
        }

        if (snprintf(sheet->symbol, sizeof(sheet->symbol), family->sheetSymbolTemplate,
                     t->symbol) >= (int)sizeof(sheet->symbol))
        {
            Fail("legacy symbol too long for '%s'", t->canonical);
            return false;
        }
        if (palette != NULL
         && snprintf(palette->symbol, sizeof(palette->symbol), family->paletteSymbolTemplate,
                     t->symbol) >= (int)sizeof(palette->symbol))
        {
            Fail("legacy symbol too long for '%s'", t->canonical);
            return false;
        }

        if (snprintf(artifact, sizeof(artifact), family->sheetArtifactTemplate, t->slug)
                >= (int)sizeof(artifact))
        {
            Fail("source artifact too long for '%s'", t->canonical);
            return false;
        }
        strcpy(sheet->sourceArtifact, artifact);
        strcpy(sheet->sourceEncoding, family->sheetEncoding);
        strcpy(sheet->canonicalRepresentation, family->sheetRepresentation);
        if (palette != NULL)
        {
            const char *paletteDir = "front_pics";
            if (t->paletteDir == PALETTE_DIR_PALETTES)
                paletteDir = "palettes";
            else if (t->paletteDir == PALETTE_DIR_BACK_PICS)
                paletteDir = "back_pics";
            if (snprintf(artifact, sizeof(artifact), "graphics/trainers/%s/%s.gbapal.lz",
                         paletteDir, t->slug) >= (int)sizeof(artifact))
            {
                Fail("source artifact too long for '%s'", t->canonical);
                return false;
            }
            strcpy(palette->sourceArtifact, artifact);
            strcpy(palette->sourceEncoding, family->paletteEncoding);
            strcpy(palette->canonicalRepresentation, family->paletteRepresentation);
        }

        sheetSize = t->sheetSize > 0 ? t->sheetSize : family->sheetDecodedSize;
        sheet->expectedDecodedSize = sheetSize;
        if (palette != NULL)
            palette->expectedDecodedSize = family->paletteDecodedSize;

        if (!DecodeArtifact(sheet->sourceArtifact, sheetSize, family->sheetEncoding,
                            sheet->encodedSha, sheet->decodedSha,
                            &sheet->encodedLength, &sheet->decodedLength))
            return false;
        if (palette != NULL
         && !DecodeArtifact(palette->sourceArtifact, family->paletteDecodedSize,
                            family->paletteEncoding,
                            palette->encodedSha, palette->decodedSha,
                            &palette->encodedLength, &palette->decodedLength))
            return false;

        strcpy(sheet->ownershipState, t->ownershipNative);
        strcpy(sheet->nativeTarget, t->ownershipNative);
        if (palette != NULL)
        {
            strcpy(palette->ownershipState, t->ownershipNative);
            strcpy(palette->nativeTarget, t->ownershipNative);
        }

        /* Primary consumer slot: the family's sheet table. */
        strcpy(sheet->slots[0].table, family->sheetTable);
        sheet->slots[0].index = t->index;
        sheet->slotCount = 1u;

        /* R8: SpriteFrameImage frame-array consumer (the live pixel surface;
         * back sheets only). */
        if (t->frames > 0)
        {
            if (strlen(t->frameArray) >= sizeof(sheet->frameArray))
            {
                Fail("frame_array too long for '%s'", t->canonical);
                return false;
            }
            strcpy(sheet->frameArray, t->frameArray);
            sheet->frames = t->frames;
        }

        if (palette != NULL)
        {
            strcpy(palette->slots[0].table, family->paletteTable);
            palette->slots[0].index = t->index;
            palette->slotCount = 1u;

            if (t->backPaletteIndex >= 0)
            {
                if (t->backPaletteIndex > 7)
                {
                    Fail("unknown trainer table ref: back_palette_index %lld for '%s' "
                         "(must be 0..7)", t->backPaletteIndex, t->canonical);
                    return false;
                }
                strcpy(palette->slots[1].table, "gTrainerBackPicPaletteTable");
                palette->slots[1].index = t->backPaletteIndex;
                palette->slotCount = 2u;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Validation (fail-closed, §18)                                       */
/* ------------------------------------------------------------------ */

static int CompareResourceById(const void *left, const void *right)
{
    const struct Resource *a = (const struct Resource *)left;
    const struct Resource *b = (const struct Resource *)right;
    return strcmp(a->id, b->id);
}

static bool ValidateFamily(struct Family *family)
{
    size_t i;
    size_t j;

    /* Canonical-name validity, duplicate ids, key collisions. */
    for (i = 0u; i < family->resourceCount; i++)
    {
        const struct Resource *a = &family->resources[i];
        Gen3ResourceKey keyA;
        enum Gen3ResourceNameStatus status;
        status = Gen3ResourceId_ValidateCanonicalName(a->id);
        if (status != GEN3_RESOURCE_NAME_VALID)
        {
            Fail("canonical id invalid (%d): %s", (int)status, a->id);
            return false;
        }
        Gen3ResourceId_DeriveKey(a->id, &keyA);
        for (j = 0u; j < i; j++)
        {
            const struct Resource *b = &family->resources[j];
            Gen3ResourceKey keyB;
            if (strcmp(a->id, b->id) == 0)
            {
                Fail("duplicate canonical id: %s", a->id);
                return false;
            }
            Gen3ResourceId_DeriveKey(b->id, &keyB);
            if (Gen3ResourceId_KeyEqual(&keyA, &keyB))
            {
                Fail("canonical key collision: %s and %s", a->id, b->id);
                return false;
            }
        }
    }

    /* Per-trainer uniqueness: canonical, slug (=> source artifact), symbol,
     * index. A repeated slug would bind two resources to the same source
     * artifact without explicit sharing -> fail closed. */
    for (i = 0u; i < family->trainerCount; i++)
    {
        const struct FamilyTrainer *a = &family->trainers[i];
        for (j = i + 1u; j < family->trainerCount; j++)
        {
            const struct FamilyTrainer *b = &family->trainers[j];
            if (strcmp(a->canonical, b->canonical) == 0)
            {
                Fail("duplicate canonical name: %s", a->canonical);
                return false;
            }
            if (strcmp(a->slug, b->slug) == 0)
            {
                Fail("duplicate source artifact (not shared): slug '%s' "
                     "used by both '%s' and '%s'", a->slug, a->canonical, b->canonical);
                return false;
            }
            if (strcmp(a->symbol, b->symbol) == 0)
            {
                Fail("duplicate GBA symbol suffix: '%s' used by both '%s' and '%s'",
                     a->symbol, a->canonical, b->canonical);
                return false;
            }
            if (a->index == b->index)
            {
                Fail("duplicate table index %lld for '%s' and '%s'",
                     a->index, a->canonical, b->canonical);
                return false;
            }
        }
    }

    /* Ownership state must be one of the two supported values. */
    for (i = 0u; i < family->resourceCount; i++)
    {
        const struct Resource *r = &family->resources[i];
        if (strcmp(r->ownershipState, "ROM_BASE_ONLY") != 0
         && strcmp(r->ownershipState, "COMPILED_PENDING_MIGRATION") != 0)
        {
            Fail("unsupported ownership_native '%s' for %s", r->ownershipState, r->id);
            return false;
        }
    }

    /* Sortability proof: ids are strictly ordered once sorted (§17). */
    {
        struct Resource sorted[MAX_RESOURCES];
        size_t n = family->resourceCount;
        if (n == 0u || n > MAX_RESOURCES)
        {
            Fail("resource count out of range (%zu)", n);
            return false;
        }
        memcpy(sorted, family->resources, n * sizeof(sorted[0]));
        qsort(sorted, n, sizeof(sorted[0]), CompareResourceById);
        for (i = 1u; i < n; i++)
        {
            if (strcmp(sorted[i - 1u].id, sorted[i].id) >= 0)
            {
                Fail("deterministic sort invariant broken");
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Emitters                                                            */
/* ------------------------------------------------------------------ */

static bool EmitHeader(struct Gen3Buffer *out, const char *descriptorName)
{
    char line[256];
    if (snprintf(line, sizeof(line),
                 "# Generated by tools/gen3_resources/trainer_family/gen_trainer_family.\n"
                 "# Do not edit by hand; edit resources/extraction/emerald/bpee01/\n"
                 "# %s and re-run the generator.\n\n", descriptorName)
            >= (int)sizeof(line))
        return false;
    return Gen3Buffer_AppendCStr(out, line);
}

static bool EmitCatalog(struct Gen3Buffer *out, const struct Family *family)
{
    size_t i;
    Gen3Buffer_AppendFormat(out, "catalog_version = %lld\n", family->catalogVersion);
    Gen3TomlWrite_String(out, "namespace", family->namespace);
    Gen3TomlWrite_String(out, "resource_api", family->resourceApi);
    Gen3TomlWrite_String(out, "game", family->game);
    Gen3TomlWrite_String(out, "rom_profile", family->romProfile);
    Gen3Buffer_AppendCStr(out, "\n");

    for (i = 0u; i < family->resourceCount; i++)
    {
        const struct Resource *r = &family->resources[i];
        Gen3TomlWrite_OpenArrayTable(out, "resources");
        Gen3TomlWrite_String(out, "id", r->id);
        Gen3TomlWrite_String(out, "type", r->type);
        Gen3TomlWrite_Integer(out, "schema", r->schema);
        Gen3Buffer_AppendFormat(out, "required_for_base = %s\n",
                                r->requiredForBase ? "true" : "false");
    }
    return true;
}

static bool EmitBindings(struct Gen3Buffer *out, const struct Family *family)
{
    size_t i;
    Gen3Buffer_AppendCStr(out, "bindings_version = 1\n");
    Gen3TomlWrite_String(out, "game", family->game);
    Gen3TomlWrite_String(out, "rom_profile", family->romProfile);
    if (strcmp(family->familyLabel, "front") == 0)
    {
        Gen3Buffer_AppendCStr(out,
            "\n# Semantic extraction bindings for the complete trainer-front family.\n"
            "# NO ROM offsets: they are derived at R7B time from the matching GBA ELF\n"
            "# symbol table via the R1A generator (gen3-elf-manifest), never hand-\n"
            "# maintained. Source encoding/representation mirror the catalog types.\n\n");
    }
    else
    {
        Gen3Buffer_AppendCStr(out,
            "\n# Semantic extraction bindings for the complete trainer-back family.\n"
            "# NO ROM offsets: they are derived at R8 time from the matching GBA ELF\n"
            "# symbol table via the R1A generator (gen3-elf-manifest), never hand-\n"
            "# maintained. Source encoding/representation mirror the catalog types.\n\n");
    }

    for (i = 0u; i < family->resourceCount; i++)
    {
        const struct Resource *r = &family->resources[i];
        Gen3TomlWrite_OpenArrayTable(out, "bindings");
        Gen3TomlWrite_String(out, "id", r->id);
        Gen3TomlWrite_String(out, "symbol", r->symbol);
        Gen3TomlWrite_String(out, "source_artifact", r->sourceArtifact);
        Gen3TomlWrite_String(out, "source_encoding", r->sourceEncoding);
        Gen3TomlWrite_String(out, "canonical_representation", r->canonicalRepresentation);
        Gen3TomlWrite_Integer(out, "expected_decoded_size", r->expectedDecodedSize);
    }
    return true;
}

static bool EmitOwnership(struct Gen3Buffer *out, const struct Family *family)
{
    size_t i;
    if (strcmp(family->familyLabel, "front") == 0)
    {
        Gen3Buffer_AppendCStr(out,
            "# Native asset-ownership declaration. Each [[resources]] block records\n"
            "# the canonical id, M0/M1 key, legacy compiled symbol, committed source\n"
            "# artifact, per-target ownership state and source hashes.\n"
            "#   native = ROM_BASE_ONLY              -> payload NOT linked natively;\n"
            "#            served via ROM_BASE provider + EmeraldResourceCompat seam\n"
            "#            (the full 93-trainer front family, R7B: all 186 payloads\n"
            "#            are served from the ROM_BASE pack at runtime).\n"
            "#   native = COMPILED_PENDING_MIGRATION -> legacy symbol still compiled\n"
            "#            for native; a future stage flips it to ROM_BASE_ONLY.\n"
            "#            (Zero such records remain for this family after R7B.)\n"
            "#   gba    = COMPILED                  -> unchanged traditional build.\n"
            "# tests/run_emerald_native_asset_isolation.sh consumes this file and\n"
            "# FAILS when the native target contradicts it.\n\n");
    }
    else
    {
        Gen3Buffer_AppendCStr(out,
            "# Native asset-ownership declaration. Each [[resources]] block records\n"
            "# the canonical id, M0/M1 key, legacy compiled symbol, committed source\n"
            "# artifact, per-target ownership state and source hashes.\n"
            "#   native = ROM_BASE_ONLY              -> payload NOT linked natively;\n"
            "#            served via ROM_BASE provider + EmeraldResourceCompat seam\n"
            "#            (the R8 back family: all 10 payloads are served from the\n"
            "#            ROM_BASE pack at runtime; the 6 alias back palettes are\n"
            "#            the R7B front normal palettes, not duplicated here).\n"
            "#   native = COMPILED_PENDING_MIGRATION -> legacy symbol still compiled\n"
            "#            for native; a future stage flips it to ROM_BASE_ONLY.\n"
            "#            (None exist for the back family.)\n"
            "#   gba    = COMPILED                  -> unchanged traditional build.\n"
            "# tests/run_emerald_native_asset_isolation.sh consumes this file and\n"
            "# FAILS when the native target contradicts it.\n\n");
    }
    Gen3Buffer_AppendCStr(out, "ownership_version = 1\n");
    Gen3TomlWrite_String(out, "game", family->game);
    Gen3TomlWrite_String(out, "rom_profile", family->romProfile);
    Gen3Buffer_AppendCStr(out, "\n");

    for (i = 0u; i < family->resourceCount; i++)
    {
        const struct Resource *r = &family->resources[i];
        char keyHex[GEN3_RESOURCE_KEY_HEX_SIZE];
        char encodedShaHex[GEN3_RESOURCE_KEY_HEX_SIZE];
        char decodedShaHex[GEN3_RESOURCE_KEY_HEX_SIZE];
        Gen3ResourceKey key;

        Gen3TomlWrite_OpenArrayTable(out, "resources");
        Gen3TomlWrite_String(out, "id", r->id);
        Gen3ResourceId_DeriveKey(r->id, &key);
        Gen3ResourceId_FormatKeyHex(&key, keyHex);
        Gen3TomlWrite_String(out, "key", keyHex);
        Gen3TomlWrite_String(out, "legacy_symbol", r->symbol);
        Gen3TomlWrite_String(out, "type", r->type);
        Gen3TomlWrite_Integer(out, "schema", r->schema);
        Gen3TomlWrite_String(out, "source_artifact", r->sourceArtifact);
        Gen3TomlWrite_Integer(out, "encoded_length", (long long)r->encodedLength);
        Gen3TomlWrite_Integer(out, "decoded_length", (long long)r->decodedLength);
        Gen3TomlWrite_String(out, "source_encoding", r->sourceEncoding);
        Gen3Util_FormatHex(r->encodedSha, GEN3_RESOURCE_KEY_SIZE, encodedShaHex);
        Gen3TomlWrite_String(out, "source_encoded_sha256", encodedShaHex);
        Gen3Util_FormatHex(r->decodedSha, GEN3_RESOURCE_KEY_SIZE, decodedShaHex);
        Gen3TomlWrite_String(out, "canonical_decoded_sha256", decodedShaHex);
        Gen3TomlWrite_String(out, "ownership_state", r->ownershipState);
        Gen3Buffer_AppendCStr(out, "\n[resources.targets]\n");
        Gen3TomlWrite_String(out, "native", r->nativeTarget);
        Gen3TomlWrite_String(out, "gba", "COMPILED");
    }
    return true;
}

struct TableSite
{
    char table[64];
    char site[256];
};

static int CompareTableSite(const void *left, const void *right)
{
    const struct TableSite *a = (const struct TableSite *)left;
    const struct TableSite *b = (const struct TableSite *)right;
    int byTable = strcmp(a->table, b->table);
    if (byTable != 0)
        return byTable;
    return strcmp(a->site, b->site);
}

static bool EmitConsumers(struct Gen3Buffer *out, const struct Family *family)
{
    struct TableSite sites[MAX_TABLE_SITES];
    size_t siteCount = 0u;
    size_t i;
    size_t n = family->resourceCount;
    bool anyFrames = false;
    char firstLine[128];

    for (i = 0u; i < n; i++)
    {
        if (family->resources[i].frames > 0)
            anyFrames = true;
    }

    if (snprintf(firstLine, sizeof(firstLine),
                 "# Machine-readable consumer map for the trainer-%s family (§13).\n",
                 family->familyLabel) >= (int)sizeof(firstLine))
    {
        Fail("consumer comment line too long");
        return false;
    }
    Gen3Buffer_AppendCStr(out, firstLine);
    if (strcmp(family->familyLabel, "front") == 0)
    {
        Gen3Buffer_AppendCStr(out,
            "#   [[table_sites]]    one row per (table, GBA source location): the\n"
            "#                      checked-in sites that read the table generically\n"
            "#                      by trainer index / pic id.\n"
            "#   [[resource_slots]] one row per (resource, table, index): every table\n"
            "#                      slot that consumes the resource. The 6 shared\n"
            "#                      front palettes each get a second row for\n"
            "#                      gTrainerBackPicPaletteTable (same resource, extra\n"
            "#                      consumer). No consumer is changed in R7A.\n");
    }
    else
    {
        Gen3Buffer_AppendCStr(out,
            "#   [[table_sites]]    one row per (table, GBA source location): the\n"
            "#                      checked-in sites that read the table generically\n"
            "#                      by trainer index / pic id. Only the tables the\n"
            "#                      family actually consumes are listed (the R8\n"
            "#                      back family: back pic table + back palette\n"
            "#                      table).\n"
            "#   [[resource_slots]] one row per (resource, table, index): every table\n"
            "#                      slot that consumes the resource. The 2 unique\n"
            "#                      back palettes (red, leaf) get their\n"
            "#                      gTrainerBackPicPaletteTable rows here; the 6\n"
            "#                      alias back-palette slots (brendan, may, rs-\n"
            "#                      brendan, rs-may, wally, steven) live in the\n"
            "#                      front family's consumers file. No consumer is\n"
            "#                      changed in R7A/R8.\n");
    }
    if (anyFrames)
    {
        Gen3Buffer_AppendCStr(out,
            "#   [[frame_slots]]    R8 back family only: one row per (resource,\n"
            "#                      frame array, frame) for the SpriteFrameImage\n"
            "#                      arrays in src/data.c that are the LIVE pixel\n"
            "#                      surface (sprite template .images ->\n"
            "#                      RequestSpriteFrameImageCopy).\n");
    }
    if (strcmp(family->familyLabel, "front") == 0)
    {
        Gen3Buffer_AppendCStr(out,
            "# Native-only runtime publication (emerald_trainer_native_compat.c)\n"
            "# applies only to brendan and is not a GBA consumer.\n\n");
    }
    else
    {
        Gen3Buffer_AppendCStr(out,
            "# Native-only runtime publication (emerald_trainer_native_compat.c)\n"
            "# applies to all back-family resources and is not a GBA consumer.\n\n");
    }
    Gen3Buffer_AppendCStr(out, "consumers_version = 1\n");
    Gen3TomlWrite_String(out, "game", family->game);
    Gen3TomlWrite_String(out, "rom_profile", family->romProfile);
    Gen3Buffer_AppendCStr(out, "\n");

    /* Per-table consumer sites (sorted bytewise by table, then site). Only the
     * tables referenced by this family's resource slots are emitted, so a
     * family never carries another family's sites. */
    {
        size_t r;
        size_t t;
        bool used[KNOWN_TABLE_COUNT];
        memset(used, 0, sizeof(used));
        for (r = 0u; r < n; r++)
        {
            size_t s;
            for (s = 0u; s < family->resources[r].slotCount; s++)
            {
                size_t k;
                for (k = 0u; k < KNOWN_TABLE_COUNT; k++)
                {
                    if (strcmp(family->resources[r].slots[s].table, kKnownTables[k]) == 0)
                        used[k] = true;
                }
            }
        }
        for (t = 0u; t < KNOWN_TABLE_COUNT; t++)
        {
            struct SiteList list;
            size_t s;
            if (!used[t])
                continue;
            list = SiteListForTable(kKnownTables[t]);
            for (s = 0u; s < list.count; s++)
            {
                if (siteCount >= MAX_TABLE_SITES)
                {
                    Fail("consumer-site table overflow");
                    return false;
                }
                strcpy(sites[siteCount].table, kKnownTables[t]);
                strncpy(sites[siteCount].site, list.sites[s], sizeof(sites[siteCount].site) - 1u);
                sites[siteCount].site[sizeof(sites[siteCount].site) - 1u] = '\0';
                siteCount++;
            }
        }
    }
    qsort(sites, siteCount, sizeof(sites[0]), CompareTableSite);
    for (i = 0u; i < siteCount; i++)
    {
        Gen3TomlWrite_OpenArrayTable(out, "table_sites");
        Gen3TomlWrite_String(out, "table", sites[i].table);
        Gen3TomlWrite_String(out, "site", sites[i].site);
    }
    Gen3Buffer_AppendCStr(out, "\n");

    /* Per-resource slots. family->resources is already sorted bytewise by id
     * (main), and per-resource slot order is fixed: front table first, back
     * table second — so this section is deterministically ordered too. */
    if (n == 0u || n > MAX_RESOURCES)
    {
        Fail("resource count out of range (%zu)", n);
        return false;
    }
    for (i = 0u; i < n; i++)
    {
        const struct Resource *r = &family->resources[i];
        size_t s;
        for (s = 0u; s < r->slotCount; s++)
        {
            Gen3TomlWrite_OpenArrayTable(out, "resource_slots");
            Gen3TomlWrite_String(out, "id", r->id);
            Gen3TomlWrite_String(out, "table", r->slots[s].table);
            Gen3TomlWrite_Integer(out, "index", r->slots[s].index);
        }
    }

    /* R8 frame_slots: SpriteFrameImage frame-array consumers (live pixel
     * surface). Resources are already sorted bytewise by id; frames ascend. */
    if (anyFrames)
    {
        Gen3Buffer_AppendCStr(out, "\n");
        for (i = 0u; i < n; i++)
        {
            const struct Resource *r = &family->resources[i];
            long long f;
            for (f = 0; f < r->frames; f++)
            {
                Gen3TomlWrite_OpenArrayTable(out, "frame_slots");
                Gen3TomlWrite_String(out, "id", r->id);
                Gen3TomlWrite_String(out, "array", r->frameArray);
                Gen3TomlWrite_Integer(out, "index", f);
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void PrintUsage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --descriptor PATH\n"
        "        [--back-descriptor PATH]\n"
        "        [--catalog-out PATH] [--bindings-out PATH]\n"
        "        [--ownership-out PATH] [--consumers-out PATH]\n"
        "        [--back-bindings-out PATH] [--back-ownership-out PATH]\n"
        "        [--back-consumers-out PATH]\n"
        "        [--check]\n"
        "\n"
        "Generates catalog.toml (union when --back-descriptor is given),\n"
        "bindings.generated.toml, ownership.generated.toml and\n"
        "trainer_front_consumers.generated.toml from the family descriptor.\n"
        "With --back-descriptor it additionally emits the R8 back-family\n"
        "bindings/ownership/consumers files (the catalog is the union of both\n"
        "families). Outputs are sorted bytewise by canonical resource id (§17).\n"
        "--check verifies the given outputs match the descriptor without writing.\n",
        argv0);
}

/* Cross-family duplicate check: the catalog is the union of both families, so
 * an id or M0/M1 key collision across families would break the registry. */
static bool CheckCrossFamily(const struct Family *a, const struct Family *b)
{
    size_t i;
    size_t j;
    for (i = 0u; i < a->resourceCount; i++)
    {
        Gen3ResourceKey keyA;
        Gen3ResourceId_DeriveKey(a->resources[i].id, &keyA);
        for (j = 0u; j < b->resourceCount; j++)
        {
            Gen3ResourceKey keyB;
            if (strcmp(a->resources[i].id, b->resources[j].id) == 0)
            {
                Fail("duplicate canonical id across families: %s", a->resources[i].id);
                return false;
            }
            Gen3ResourceId_DeriveKey(b->resources[j].id, &keyB);
            if (Gen3ResourceId_KeyEqual(&keyA, &keyB))
            {
                Fail("canonical key collision across families: %s and %s",
                     a->resources[i].id, b->resources[j].id);
                return false;
            }
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *descriptorPath = NULL;
    const char *backDescriptorPath = NULL;
    const char *catalogOut = NULL;
    const char *bindingsOut = NULL;
    const char *ownershipOut = NULL;
    const char *consumersOut = NULL;
    const char *backBindingsOut = NULL;
    const char *backOwnershipOut = NULL;
    const char *backConsumersOut = NULL;
    int doCheck = 0;
    int i;
    struct Family family;
    struct Family backFamily;
    struct Gen3Buffer catalog;
    struct Gen3Buffer bindings;
    struct Gen3Buffer ownership;
    struct Gen3Buffer consumers;
    struct Gen3Buffer backBindings;
    struct Gen3Buffer backOwnership;
    struct Gen3Buffer backConsumers;
    char errbuf[512];

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--descriptor") == 0 && i + 1 < argc)
            descriptorPath = argv[++i];
        else if (strcmp(argv[i], "--back-descriptor") == 0 && i + 1 < argc)
            backDescriptorPath = argv[++i];
        else if (strcmp(argv[i], "--catalog-out") == 0 && i + 1 < argc)
            catalogOut = argv[++i];
        else if (strcmp(argv[i], "--bindings-out") == 0 && i + 1 < argc)
            bindingsOut = argv[++i];
        else if (strcmp(argv[i], "--ownership-out") == 0 && i + 1 < argc)
            ownershipOut = argv[++i];
        else if (strcmp(argv[i], "--consumers-out") == 0 && i + 1 < argc)
            consumersOut = argv[++i];
        else if (strcmp(argv[i], "--back-bindings-out") == 0 && i + 1 < argc)
            backBindingsOut = argv[++i];
        else if (strcmp(argv[i], "--back-ownership-out") == 0 && i + 1 < argc)
            backOwnershipOut = argv[++i];
        else if (strcmp(argv[i], "--back-consumers-out") == 0 && i + 1 < argc)
            backConsumersOut = argv[++i];
        else if (strcmp(argv[i], "--check") == 0)
            doCheck = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else
        {
            Fail("unknown argument: %s", argv[i]);
            return 1;
        }
    }

    if (descriptorPath == NULL)
    {
        Fail("--descriptor is required");
        return 1;
    }
    if (!doCheck && (catalogOut == NULL || bindingsOut == NULL
                     || ownershipOut == NULL || consumersOut == NULL))
    {
        Fail("all four --*-out paths are required (or use --check)");
        return 1;
    }
    if (backDescriptorPath != NULL && !doCheck
     && (backBindingsOut == NULL || backOwnershipOut == NULL || backConsumersOut == NULL))
    {
        Fail("--back-descriptor requires the three --back-*-out paths (or use --check)");
        return 1;
    }

    if (!ParseFamily(descriptorPath, &family))
        return gExitCode ? gExitCode : 1;
    if (!BuildResources(&family))
        return gExitCode ? gExitCode : 1;
    if (!ValidateFamily(&family))
        return gExitCode ? gExitCode : 1;

    memset(&backFamily, 0, sizeof(backFamily));
    if (backDescriptorPath != NULL)
    {
        if (!ParseFamily(backDescriptorPath, &backFamily))
            return gExitCode ? gExitCode : 1;
        if (!BuildResources(&backFamily))
            return gExitCode ? gExitCode : 1;
        if (!ValidateFamily(&backFamily))
            return gExitCode ? gExitCode : 1;
        if (!CheckCrossFamily(&family, &backFamily))
            return gExitCode ? gExitCode : 1;
    }

    /* §17: every emitted file is sorted bytewise by canonical id, independent of
     * descriptor order. ValidateFamily already proved the strict order exists;
     * sort in place once so all emitters share it. */
    qsort(family.resources, family.resourceCount, sizeof(family.resources[0]),
          CompareResourceById);
    if (backFamily.resourceCount > 0)
        qsort(backFamily.resources, backFamily.resourceCount,
              sizeof(backFamily.resources[0]), CompareResourceById);

    {
        const char *frontName = descriptorPath;
        const char *slash = strrchr(frontName, '/');
        if (slash != NULL)
            frontName = slash + 1;
        Gen3Buffer_Init(&catalog, 1u << 20);
        Gen3Buffer_Init(&bindings, 1u << 20);
        Gen3Buffer_Init(&ownership, 1u << 20);
        Gen3Buffer_Init(&consumers, 1u << 20);
        EmitHeader(&catalog, frontName);
        EmitHeader(&bindings, frontName);
        EmitHeader(&ownership, frontName);
        EmitHeader(&consumers, frontName);
        EmitCatalog(&catalog, &family);
        if (backFamily.resourceCount > 0)
            EmitCatalog(&catalog, &backFamily);
        EmitBindings(&bindings, &family);
        EmitOwnership(&ownership, &family);
        EmitConsumers(&consumers, &family);
    }

    if (backFamily.resourceCount > 0)
    {
        const char *backName = backDescriptorPath;
        const char *slash = strrchr(backName, '/');
        if (slash != NULL)
            backName = slash + 1;
        Gen3Buffer_Init(&backBindings, 1u << 20);
        Gen3Buffer_Init(&backOwnership, 1u << 20);
        Gen3Buffer_Init(&backConsumers, 1u << 20);
        EmitHeader(&backBindings, backName);
        EmitHeader(&backOwnership, backName);
        EmitHeader(&backConsumers, backName);
        EmitBindings(&backBindings, &backFamily);
        EmitOwnership(&backOwnership, &backFamily);
        EmitConsumers(&backConsumers, &backFamily);
    }

    if (doCheck)
    {
        int mismatch = 0;
        struct
        {
            const char *path;
            struct Gen3Buffer *buf;
        } targets[7] = {
            { catalogOut, &catalog },
            { bindingsOut, &bindings },
            { ownershipOut, &ownership },
            { consumersOut, &consumers },
            { backBindingsOut, backFamily.resourceCount > 0 ? &backBindings : NULL },
            { backOwnershipOut, backFamily.resourceCount > 0 ? &backOwnership : NULL },
            { backConsumersOut, backFamily.resourceCount > 0 ? &backConsumers : NULL },
        };
        int t;
        for (t = 0; t < 7; t++)
        {
            struct Gen3Buffer existing; /* ReadFile owns init */
            if (targets[t].path == NULL || targets[t].buf == NULL)
                continue;
            if (!Gen3Util_ReadFile(targets[t].path, &existing, errbuf, sizeof(errbuf)))
            {
                Fail("%s unreadable: %s", targets[t].path, errbuf);
                mismatch = 1;
            }
            else if (existing.length != targets[t].buf->length
                     || memcmp(existing.data, targets[t].buf->data, existing.length) != 0)
            {
                Fail("%s does not match generated output", targets[t].path);
                mismatch = 1;
            }
            Gen3Buffer_Destroy(&existing);
        }
        if (!mismatch)
            fprintf(stderr, "check passed: %zu resources (%zu front + %zu back), "
                            "%zu/%zu trainers, %zu byte total output\n",
                    family.resourceCount + backFamily.resourceCount,
                    family.resourceCount, backFamily.resourceCount,
                    family.trainerCount, backFamily.trainerCount,
                    catalog.length + bindings.length + ownership.length + consumers.length
                    + backBindings.length + backOwnership.length + backConsumers.length);
        /* Destroy on both exits (the sanitizer suite runs with
         * detect_leaks=1, so an early return must not leak). */
        Gen3Buffer_Destroy(&catalog);
        Gen3Buffer_Destroy(&bindings);
        Gen3Buffer_Destroy(&ownership);
        Gen3Buffer_Destroy(&consumers);
        if (backFamily.resourceCount > 0)
        {
            Gen3Buffer_Destroy(&backBindings);
            Gen3Buffer_Destroy(&backOwnership);
            Gen3Buffer_Destroy(&backConsumers);
        }
        return mismatch ? 1 : 0;
    }

    if (!Gen3Util_WriteFile(catalogOut, catalog.data, catalog.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(bindingsOut, bindings.data, bindings.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(ownershipOut, ownership.data, ownership.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(consumersOut, consumers.data, consumers.length, errbuf, sizeof(errbuf)))
    {
        Fail("cannot write outputs: %s", errbuf);
        return 1;
    }
    if (backFamily.resourceCount > 0
     && (!Gen3Util_WriteFile(backBindingsOut, backBindings.data, backBindings.length,
                             errbuf, sizeof(errbuf))
      || !Gen3Util_WriteFile(backOwnershipOut, backOwnership.data, backOwnership.length,
                             errbuf, sizeof(errbuf))
      || !Gen3Util_WriteFile(backConsumersOut, backConsumers.data, backConsumers.length,
                             errbuf, sizeof(errbuf))))
    {
        Fail("cannot write back outputs: %s", errbuf);
        return 1;
    }

    Gen3Buffer_Destroy(&catalog);
    Gen3Buffer_Destroy(&bindings);
    Gen3Buffer_Destroy(&ownership);
    Gen3Buffer_Destroy(&consumers);
    if (backFamily.resourceCount > 0)
    {
        Gen3Buffer_Destroy(&backBindings);
        Gen3Buffer_Destroy(&backOwnership);
        Gen3Buffer_Destroy(&backConsumers);
    }

    fprintf(stderr, "wrote %zu resources (%zu front + %zu back, %zu/%zu trainers):\n"
                    "  %s\n  %s\n  %s\n  %s\n",
            family.resourceCount + backFamily.resourceCount,
            family.resourceCount, backFamily.resourceCount,
            family.trainerCount, backFamily.trainerCount,
            catalogOut, bindingsOut, ownershipOut, consumersOut);
    if (backFamily.resourceCount > 0)
        fprintf(stderr, "  %s\n  %s\n  %s\n", backBindingsOut, backOwnershipOut, backConsumersOut);
    return 0;
}
