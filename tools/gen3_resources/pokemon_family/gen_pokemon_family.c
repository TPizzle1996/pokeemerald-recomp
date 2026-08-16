/*
 * gen_pokemon_family — R9 Pokémon battle-graphics family generator.
 *
 * Consumes the machine-readable inventory emitted by gen_pokemon_inventory.py
 * (resources/extraction/emerald/bpee01/pokemon_battle/inventory.generated.toml)
 * and emits five deterministic, bytewise-sorted metadata files:
 *
 *   catalog.generated.toml          — full family resource catalog (1607).
 *   bindings.generated.toml         — semantic extraction bindings (1607).
 *   ownership.generated.toml        — native/GBA ownership per resource (1607).
 *   consumers.generated.toml        — machine-readable consumer map (1760 slots).
 *   species_mapping.generated.toml  — total index mapping: every table slot
 *                                     (table, index, species) -> canonical id.
 *
 * The four families (front sheet, back sheet, normal palette, shiny palette)
 * are the four gMon*PicTable/gMon*PaletteTable tables. The inventory already
 * proved index == species id for all 1760 slots; this generator derives the
 * canonical ids, resolves cross-family aliases, verifies every payload against
 * the committed source artifacts (strict GBA LZ77 decode + SHA-256) and emits
 * the metadata in the same shapes the R7A/R8 trainer family uses, so the
 * existing import pipeline (manifest.production.toml, emerald_resource_import,
 * resource pack writer/provider) consumes the new family without changes.
 *
 * Canonical-name rule (R9 §4): canonical = the INCBIN artifact directory,
 * verbatim (underscores kept): graphics/pokemon/<slug>/... -> slug. The slug
 * is thus traceable to the source asset path with no transformation; the ID
 * templates build
 *   emerald:pokemon/<slug>/battle/front/sheet
 *   emerald:pokemon/<slug>/battle/back/sheet
 *   emerald:pokemon/<slug>/battle/normal-palette
 *   emerald:pokemon/<slug>/battle/shiny-palette
 *
 * Cross-family aliases (the tables ARE the truth; no handwritten mapping):
 *   gMonBackPicTable[EGG=412]      -> gMonStillFrontPic_Egg  (still-front
 *                                     family: external alias, stays compiled
 *                                     on native, no canonical, slot recorded
 *                                     in the external_slots section).
 *   gMonShinyPaletteTable[EGG=412] -> gMonPalette_Egg        (normal palette
 *                                     family: alias of the normal-palette
 *                                     canonical, not duplicated).
 *   gMonPaletteTable[UNOWN..QMARK] -> gMonPalette_Unown      (28 slots share
 *                                     one payload; slot-level alias).
 *   gMonShinyPaletteTable[unown..] -> gMonShinyPalette_Unown (same).
 *   *_DoubleQuestionMark           -> 25 dead OLD_UNOWN_B..Z slots per table.
 * Every multi-slot payload is one resource with multiple slots; only
 * payload-owning resources appear in catalog/bindings/ownership.
 *
 * Determinism (§17): output order independent of inventory order. Resources
 * sorted bytewise by canonical id; per-family slot rows by table then index.
 * Running the generator twice produces byte-identical files.
 *
 * Fail-closed validation (§18): every check below reports a structured
 * diagnostic ("FAIL: <message>") and returns a nonzero exit code.
 *
 * Usage:
 *   gen_pokemon_family --inventory PATH \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH --species-map-out PATH
 *   gen_pokemon_family --inventory PATH --check \
 *       --catalog-out PATH --bindings-out PATH \
 *       --ownership-out PATH --consumers-out PATH --species-map-out PATH
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

#define MAX_RESOURCES_PER_FAMILY 512u
#define MAX_SLOTS_PER_RESOURCE 32u
#define MAX_TABLE_SITES 96u /* 44 front + 15 back + 6 palette + 3 shiny = 68 */
#define ARTIFACT_BUF 512u
#define SYMBOL_BUF 192u
#define SLUG_BUF 128u
#define SPECIES_TOKEN_BUF 64u

#define POKEMON_NAMESPACE "emerald"
#define POKEMON_TYPE_SHEET "tile-graphics"
#define POKEMON_TYPE_PALETTE "palette"
#define POKEMON_SCHEMA 1
#define POKEMON_SHEET_REPR "gba-4bpp-tiles"
#define POKEMON_PALETTE_REPR "gba-bgr555-palette"
#define POKEMON_ENCODING "gba-lz77"

#define SLOT_COUNT_PER_TABLE 440u
#define MON_PIC_SIZE 2048u      /* .size field value (one frame), not payload */

/* Symbol prefix -> owning kind. Symbols of one family's table that carry
 * another family's prefix are cross-family aliases; anything else is an
 * external alias (only gMonStillFrontPic_Egg exists today). */
enum Kind
{
    KIND_FRONT_SHEET,
    KIND_BACK_SHEET,
    KIND_NORMAL_PALETTE,
    KIND_SHINY_PALETTE,
    KIND_COUNT,
};

struct Slot
{
    long long index;
    char species[SPECIES_TOKEN_BUF];
};

struct PokemonResource
{
    enum Kind kind;
    char id[GEN3_RESOURCE_NAME_MAX + 1u];
    char symbol[SYMBOL_BUF];
    char slug[SLUG_BUF];
    char artifact[ARTIFACT_BUF];
    char encoding[24];
    char representation[32];
    long long expectedDecodedSize;   /* derived from the artifact's LZ header */
    size_t encodedLength;
    size_t decodedLength;
    uint8_t encodedSha[GEN3_RESOURCE_KEY_SIZE];
    uint8_t decodedSha[GEN3_RESOURCE_KEY_SIZE];
    char aliasOf[GEN3_RESOURCE_NAME_MAX + 1u]; /* "" = payload-owning */
    bool crossFamily;      /* symbol owned by another battle kind */
    bool externalAlias;    /* symbol owned by no battle kind */
    size_t declaredSlotCount; /* from the inventory (must match the join) */
    struct Slot slots[MAX_SLOTS_PER_RESOURCE];
    size_t slotCount;
};

struct PokemonFamily
{
    enum Kind kind;
    char kindName[24];      /* "front_sheet" ... */
    char table[64];         /* gMonFrontPicTable ... */
    char idSegment[24];     /* "front/sheet" ... */
    char symbolPrefix[24];  /* "gMonFrontPic_" ... */
    size_t resourceCount;
    struct PokemonResource resources[MAX_RESOURCES_PER_FAMILY];
};

/* Fixed emission order for the four families. resourceCount and resources are
 * zero-initialized here and re-stamped per run in main(). */
static const struct PokemonFamily kFamilies[KIND_COUNT] =
{
    [KIND_FRONT_SHEET] = { .kind = KIND_FRONT_SHEET,
                           .kindName = "front_sheet",
                           .table = "gMonFrontPicTable",
                           .idSegment = "front/sheet",
                           .symbolPrefix = "gMonFrontPic_" },
    [KIND_BACK_SHEET] = { .kind = KIND_BACK_SHEET,
                          .kindName = "back_sheet",
                          .table = "gMonBackPicTable",
                          .idSegment = "back/sheet",
                          .symbolPrefix = "gMonBackPic_" },
    [KIND_NORMAL_PALETTE] = { .kind = KIND_NORMAL_PALETTE,
                              .kindName = "normal_palette",
                              .table = "gMonPaletteTable",
                              .idSegment = "normal-palette",
                              .symbolPrefix = "gMonPalette_" },
    [KIND_SHINY_PALETTE] = { .kind = KIND_SHINY_PALETTE,
                             .kindName = "shiny_palette",
                             .table = "gMonShinyPaletteTable",
                             .idSegment = "shiny-palette",
                             .symbolPrefix = "gMonShinyPalette_" },
};

/* External-alias allowlist: symbols referenced by a battle table but owned by
 * a family this generator does not migrate. The slot keeps its compiled
 * payload on native; the consumers map records it in external_slots. */
static const char *const kExternalAliasAllowlist[] =
{
    "gMonStillFrontPic_Egg",   /* gMonBackPicTable[EGG] (still-front family) */
};

/* Consumer-site inventory (GBA build): the checked-in source locations that
 * read each table generically by species id, sorted bytewise for
 * deterministic output. Fixed audit artifact (R9 Stage 1 census + consumer
 * audit); the machine-readable output is derived from it. rom_header_gf.c
 * rows are layout-compat address embeds with no runtime read; they are
 * listed too (like the R7A dead-site precedent) so the map is complete. */
static const char *const kTableSitesFront[] =
{
    "src/battle_anim_mons.c:2118",
    "src/battle_anim_mons.c:2124",
    "src/battle_gfx_sfx_util.c:600",
    "src/battle_gfx_sfx_util.c:990",
    "src/contest_painting.c:371",
    "src/contest_util.c:2602",
    "src/contest_util.c:2604",
    "src/contest_util.c:899",
    "src/contest_util.c:907",
    "src/decompress.c:100",
    "src/decompress.c:104",
    "src/decompress.c:310",
    "src/decompress.c:331",
    "src/decompress.c:335",
    "src/decompress.c:350",
    "src/decompress.c:361",
    "src/decompress.c:370",
    "src/decompress.c:393",
    "src/decompress.c:397",
    "src/decompress.c:67",
    "src/decompress.c:77",
    "src/egg_hatch.c:447",
    "src/evolution_scene.c:262",
    "src/evolution_scene.c:277",
    "src/evolution_scene.c:354",
    "src/evolution_scene.c:426",
    "src/evolution_scene.c:490",
    "src/menu_specialized.c:1078",
    "src/pokeblock_feed.c:730",
    "src/pokemon_storage_system.c:3981",
    "src/pokemon_summary_screen.c:3885",
    "src/pokemon_summary_screen.c:3890",
    "src/pokemon_summary_screen.c:3900",
    "src/pokemon_summary_screen.c:3905",
    "src/pokemon_summary_screen.c:3913",
    "src/pokemon_summary_screen.c:3918",
    "src/pokenav_conditions.c:537",
    "src/rom_header_gf.c:103",
    "src/trade.c:2810",
    "src/trade.c:2812",
    "src/trade.c:3807",
    "src/trade.c:4304",
    "src/trainer_pokemon_sprites.c:67",
    "src/trainer_pokemon_sprites.c:69",
};

static const char *const kTableSitesBack[] =
{
    "src/battle_anim_mons.c:2134",
    "src/battle_anim_mons.c:2140",
    "src/battle_gfx_sfx_util.c:656",
    "src/battle_gfx_sfx_util.c:662",
    "src/battle_gfx_sfx_util.c:961",
    "src/battle_gfx_sfx_util.c:980",
    "src/contest.c:3127",
    "src/contest.c:3129",
    "src/contest_painting.c:380",
    "src/decompress.c:329",
    "src/decompress.c:391",
    "src/decompress.c:98",
    "src/rom_header_gf.c:104",
    "src/trainer_pokemon_sprites.c:74",
    "src/trainer_pokemon_sprites.c:76",
};

static const char *const kTableSitesPalette[] =
{
    "src/field_effect.c:922",
    "src/field_effect.c:923",
    "src/pokemon.c:6519",
    "src/pokemon.c:6525",
    "src/pokemon.c:6544",
    "src/rom_header_gf.c:105",
};

static const char *const kTableSitesShiny[] =
{
    "src/pokemon.c:6523",
    "src/pokemon.c:6542",
    "src/rom_header_gf.c:106",
};

struct SiteList
{
    const char *const *sites;
    size_t count;
};

static struct SiteList SiteListForTable(const char *tableName)
{
    struct SiteList list = { NULL, 0u };
    size_t i;
    for (i = 0u; i < KIND_COUNT; i++)
    {
        if (strcmp(tableName, kFamilies[i].table) == 0)
        {
            switch (kFamilies[i].kind)
            {
            case KIND_FRONT_SHEET:
                list.sites = kTableSitesFront;
                list.count = sizeof(kTableSitesFront) / sizeof(kTableSitesFront[0]);
                break;
            case KIND_BACK_SHEET:
                list.sites = kTableSitesBack;
                list.count = sizeof(kTableSitesBack) / sizeof(kTableSitesBack[0]);
                break;
            case KIND_NORMAL_PALETTE:
                list.sites = kTableSitesPalette;
                list.count = sizeof(kTableSitesPalette) / sizeof(kTableSitesPalette[0]);
                break;
            case KIND_SHINY_PALETTE:
                list.sites = kTableSitesShiny;
                list.count = sizeof(kTableSitesShiny) / sizeof(kTableSitesShiny[0]);
                break;
            default:
                break;
            }
            break;
        }
    }
    return list;
}

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

static bool GetStringRequired(const struct Gen3TomlMap *map, const char *key,
                              char *out, size_t outSize, const char *what)
{
    const char *value = NULL;
    if (!Gen3Toml_GetString(map, key, &value) || value == NULL)
    {
        Fail("missing %s ('%s')", what, key);
        return false;
    }
    if (strlen(value) >= outSize)
    {
        Fail("%s ('%s') too long", what, key);
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
        Fail("missing %s ('%s')", what, key);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Inventory parsing                                                   */
/* ------------------------------------------------------------------ */

static struct PokemonResource *FindBySymbol(struct PokemonFamily *families,
                                            enum Kind kind, const char *symbol);

static enum Kind KindFromName(const char *name)
{
    size_t i;
    for (i = 0u; i < KIND_COUNT; i++)
    {
        if (strcmp(kFamilies[i].kindName, name) == 0)
            return kFamilies[i].kind;
    }
    return KIND_COUNT;
}

/* graphics/pokemon/<slug>/<file> -> <slug>, verbatim. */
static bool SlugFromArtifact(const char *artifact, char *out, size_t outSize)
{
    static const char prefix[] = "graphics/pokemon/";
    const char *start;
    const char *slash;
    size_t len;
    if (strncmp(artifact, prefix, sizeof(prefix) - 1u) != 0)
    {
        Fail("artifact not under graphics/pokemon/: %s", artifact);
        return false;
    }
    start = artifact + sizeof(prefix) - 1u;
    slash = strrchr(start, '/');
    if (slash == NULL || slash == start)
    {
        Fail("artifact path has no directory component: %s", artifact);
        return false;
    }
    len = (size_t)(slash - start);
    if (len == 0u || len >= outSize)
    {
        Fail("artifact slug out of range: %s", artifact);
        return false;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

/* Parse one [[resources]] group from the inventory into the family of its
 * kind. Slot tables arrive in a separate flat [[slots]] array (the shared
 * TOML subset has no nested table headers) and are joined by ParseSlots. */
static bool ParseResources(const struct Gen3TomlDocument *doc,
                           struct PokemonFamily *families)
{
    size_t count = Gen3Toml_GetArrayCount(&doc->root, "resources");
    size_t i;
    for (i = 0u; i < count; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&doc->root, "resources", i);
        struct PokemonResource *r = NULL;
        char kindName[24];
        char symbol[SYMBOL_BUF];
        char artifact[ARTIFACT_BUF];
        long long slotCount = 0;
        enum Kind kind;

        if (item == NULL
         || !GetStringRequired(item, "kind", kindName, sizeof(kindName), "resource kind")
         || !GetStringRequired(item, "symbol", symbol, sizeof(symbol), "resource symbol")
         || !GetStringRequired(item, "source_artifact", artifact, sizeof(artifact), "source_artifact")
         || !GetIntegerRequired(item, "slot_count", &slotCount, "slot_count"))
        {
            return false;
        }
        kind = KindFromName(kindName);
        if (kind == KIND_COUNT)
        {
            Fail("unknown resource kind '%s'", kindName);
            return false;
        }
        if (slotCount <= 0 || (size_t)slotCount > MAX_SLOTS_PER_RESOURCE)
        {
            Fail("slot_count out of range for %s (%lld)", symbol, slotCount);
            return false;
        }
        if (families[kind].resourceCount >= MAX_RESOURCES_PER_FAMILY)
        {
            Fail("too many resources for family '%s'", kFamilies[kind].kindName);
            return false;
        }

        r = &families[kind].resources[families[kind].resourceCount++];
        memset(r, 0, sizeof(*r));
        r->kind = kind;
        strcpy(r->symbol, symbol);
        strcpy(r->artifact, artifact);
        strcpy(r->encoding, POKEMON_ENCODING);
        r->declaredSlotCount = (size_t)slotCount;
        strcpy(r->representation,
               kind == KIND_NORMAL_PALETTE || kind == KIND_SHINY_PALETTE
                   ? POKEMON_PALETTE_REPR : POKEMON_SHEET_REPR);
        if (!SlugFromArtifact(artifact, r->slug, sizeof(r->slug)))
            return false;
        /* Build in a stack buffer first: the format arguments alias members
         * of the same containing struct, which -Wrestrict cannot prove
         * disjoint from r->id. */
        {
            char idBuf[GEN3_RESOURCE_NAME_MAX + 1u];
            int written = snprintf(idBuf, sizeof(idBuf), "%s:pokemon/%s/battle/%s",
                                   POKEMON_NAMESPACE, r->slug,
                                   kFamilies[kind].idSegment);
            if (written < 0 || (size_t)written >= sizeof(idBuf))
            {
                Fail("canonical id exceeds buffer for %s", r->slug);
                return false;
            }
            strcpy(r->id, idBuf);
        }
    }
    return true;
}

/* Join the flat top-level [[slots]] array onto the resources by (kind,
 * symbol). Slots must be strictly index-ascending per resource (the inventory
 * emits them that way; the consumers emitter's slot rows depend on it for
 * bytewise determinism). */
static bool ParseSlots(const struct Gen3TomlDocument *doc,
                       struct PokemonFamily *families)
{
    size_t count = Gen3Toml_GetArrayCount(&doc->root, "slots");
    size_t i;
    for (i = 0u; i < count; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&doc->root, "slots", i);
        char kindName[24];
        char symbol[SYMBOL_BUF];
        char species[SPECIES_TOKEN_BUF];
        long long index = -1;
        enum Kind kind;
        struct PokemonResource *r;

        if (item == NULL
         || !GetStringRequired(item, "kind", kindName, sizeof(kindName), "slot kind")
         || !GetStringRequired(item, "symbol", symbol, sizeof(symbol), "slot symbol")
         || !GetStringRequired(item, "species", species, sizeof(species), "slot species")
         || !GetIntegerRequired(item, "index", &index, "slot index"))
        {
            return false;
        }
        kind = KindFromName(kindName);
        if (kind == KIND_COUNT)
        {
            Fail("unknown slot kind '%s'", kindName);
            return false;
        }
        r = FindBySymbol(families, kind, symbol);
        if (r == NULL)
        {
            Fail("slot references unknown resource symbol: %s", symbol);
            return false;
        }
        if (index < 0 || (unsigned long long)index >= SLOT_COUNT_PER_TABLE)
        {
            Fail("slot index out of range in %s: %lld", symbol, index);
            return false;
        }
        if (r->slotCount > 0u
         && r->slots[r->slotCount - 1u].index >= index)
        {
            Fail("slots not strictly index-ascending for %s (determinism "
                 "invariant)", symbol);
            return false;
        }
        if (r->slotCount >= MAX_SLOTS_PER_RESOURCE)
        {
            Fail("too many slots for %s", symbol);
            return false;
        }
        r->slots[r->slotCount].index = index;
        strcpy(r->slots[r->slotCount].species, species);
        r->slotCount++;
    }
    return true;
}

/* Fail-closed join check: every resource's declared slot_count must equal the
 * number of slots the [[slots]] array actually attached. */
static bool ValidateSlotJoins(struct PokemonFamily *families)
{
    enum Kind k;
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            const struct PokemonResource *r = &families[k].resources[i];
            if (r->slotCount != r->declaredSlotCount)
            {
                Fail("slot join mismatch for %s: inventory declares %zu, "
                     "[[slots]] provides %zu", r->symbol, r->declaredSlotCount,
                     r->slotCount);
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Payload verification (strict GBA LZ77 + SHA-256)                    */
/* ------------------------------------------------------------------ */

/* Read the GBA LZ77 header size (bytes 1..3, little-endian u24) and verify
 * the family invariants, derived mechanically from the committed artifacts:
 *   sheets   -> positive multiple of MON_PIC_SIZE (2048). The real family has
 *               2048 (back, 1 frame), 4096 (front 2-frame anim / Deoxys
 *               back), and 8192 (Castform anim/back: 4 weather frames).
 *   palettes -> positive multiple of 32 (16 colors). Real family: 32
 *               (single palette) and 128 (Castform normal/shiny: 4 weather
 *               palettes). No size is hardcoded per species; the header is
 *               the truth and the compat seam's per-entry expectedSize
 *               override carries it through unchanged. */
static bool CheckLzHeader(const uint8_t *data, size_t length, enum Kind kind,
                          long long *outSize, const char *symbol)
{
    long long size;
    if (length < 4u || (data[0] & 0x0Fu) != 0u)
    {
        Fail("not a GBA LZ77 stream: %s", symbol);
        return false;
    }
    size = (long long)data[1] | ((long long)data[2] << 8) | ((long long)data[3] << 16);
    if (kind == KIND_NORMAL_PALETTE || kind == KIND_SHINY_PALETTE)
    {
        if (size <= 0 || size % 32 != 0)
        {
            Fail("palette decodes to %lld bytes (expected a 32-byte "
                 "multiple): %s", size, symbol);
            return false;
        }
    }
    else
    {
        if (size % MON_PIC_SIZE != 0 || size <= 0)
        {
            Fail("sheet decodes to %lld bytes (expected a MON_PIC_SIZE "
                 "multiple): %s", size, symbol);
            return false;
        }
    }
    *outSize = size;
    return true;
}

static bool DecodeArtifact(struct PokemonResource *r)
{
    struct Gen3Buffer encoded;
    struct Gen3Buffer decoded;
    struct Gen3Sha256Context ctx;
    size_t decodedSize = 0u;
    long long expected;
    char errbuf[512];
    enum Gen3Lz77Result result;

    if (!Gen3Util_ReadFile(r->artifact, &encoded, errbuf, sizeof(errbuf)))
    {
        Fail("source artifact missing: %s (%s)", r->artifact, errbuf);
        Gen3Buffer_Destroy(&encoded);
        return false;
    }
    if (!CheckLzHeader((const uint8_t *)encoded.data, encoded.length, r->kind,
                       &expected, r->symbol))
    {
        Gen3Buffer_Destroy(&encoded);
        return false;
    }
    if (expected > 0x7FFFFFFFLL)
    {
        Fail("decoded size overflow: %s", r->symbol);
        Gen3Buffer_Destroy(&encoded);
        return false;
    }
    Gen3Buffer_Init(&decoded, (size_t)expected);
    result = Gen3Lz77_Decode((const uint8_t *)encoded.data, encoded.length,
                             (uint8_t *)decoded.data, (size_t)expected, &decodedSize);
    if (result != GEN3_LZ77_OK)
    {
        Fail("LZ77 decode failed for %s (result %d)", r->symbol, (int)result);
        Gen3Buffer_Destroy(&encoded);
        Gen3Buffer_Destroy(&decoded);
        return false;
    }
    if ((long long)decodedSize != expected)
    {
        Fail("decoded size conflict for %s: stream declares %zu bytes, "
             "header declares %lld", r->symbol, decodedSize, expected);
        Gen3Buffer_Destroy(&encoded);
        Gen3Buffer_Destroy(&decoded);
        return false;
    }

    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, encoded.data, encoded.length);
    Gen3Sha256_Final(&ctx, r->encodedSha);

    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, (uint8_t *)decoded.data, decodedSize);
    Gen3Sha256_Final(&ctx, r->decodedSha);

    r->encodedLength = encoded.length;
    r->decodedLength = decodedSize;
    r->expectedDecodedSize = expected;

    Gen3Buffer_Destroy(&encoded);
    Gen3Buffer_Destroy(&decoded);
    return true;
}

/* ------------------------------------------------------------------ */
/* Cross-family alias resolution                                       */
/* ------------------------------------------------------------------ */

static struct PokemonResource *FindBySymbol(struct PokemonFamily *families,
                                            enum Kind kind, const char *symbol)
{
    size_t i;
    for (i = 0u; i < families[kind].resourceCount; i++)
    {
        if (strcmp(families[kind].resources[i].symbol, symbol) == 0)
            return &families[kind].resources[i];
    }
    return NULL;
}

static enum Kind OwningKindOfSymbol(const char *symbol)
{
    size_t i;
    for (i = 0u; i < KIND_COUNT; i++)
    {
        const char *prefix = kFamilies[i].symbolPrefix;
        size_t len = strlen(prefix);
        if (strncmp(symbol, prefix, len) == 0)
            return kFamilies[i].kind;
    }
    return KIND_COUNT;
}

static bool ResolveAliases(struct PokemonFamily *families)
{
    enum Kind k;
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            struct PokemonResource *r = &families[k].resources[i];
            enum Kind owner = OwningKindOfSymbol(r->symbol);
            if (owner == k)
                continue;                       /* payload-owning */
            if (owner != KIND_COUNT)
            {
                /* Cross-family alias: the owning family has the canonical. */
                struct PokemonResource *own =
                    FindBySymbol(families, owner, r->symbol);
                if (own == NULL)
                {
                    Fail("cross-family alias %s: owning family '%s' has no "
                         "resource for it", r->symbol, kFamilies[owner].kindName);
                    return false;
                }
                strcpy(r->aliasOf, own->id);
                r->crossFamily = true;
            }
            else
            {
                /* External alias: not owned by any battle kind. Allowlisted
                 * only (fail closed on anything new). */
                size_t w;
                bool allowed = false;
                for (w = 0u; w < sizeof(kExternalAliasAllowlist)
                                    / sizeof(kExternalAliasAllowlist[0]); w++)
                {
                    if (strcmp(r->symbol, kExternalAliasAllowlist[w]) == 0)
                    {
                        allowed = true;
                        break;
                    }
                }
                if (!allowed)
                {
                    Fail("external alias %s is not in the allowlist (new "
                         "cross-family reference needs a policy decision)",
                         r->symbol);
                    return false;
                }
                r->externalAlias = true;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Validation (fail-closed, §18)                                       */
/* ------------------------------------------------------------------ */

struct ValidatedResource
{
    const struct PokemonResource *resource;
    Gen3ResourceKey key;
};

static int CompareResourceById(const void *left, const void *right)
{
    const struct PokemonResource *a = (const struct PokemonResource *)left;
    const struct PokemonResource *b = (const struct PokemonResource *)right;
    return strcmp(a->id, b->id);
}

static bool ValidateFamily(struct PokemonFamily *families)
{
    struct ValidatedResource payloads[KIND_COUNT * MAX_RESOURCES_PER_FAMILY];
    size_t payloadCount = 0u;
    enum Kind k;

    /* Collect payload-owning resources (aliases carry no published id) and
     * derive each key exactly once. */
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            const struct PokemonResource *r = &families[k].resources[i];
            enum Gen3ResourceNameStatus status;
            if (r->crossFamily || r->externalAlias)
                continue;
            status = Gen3ResourceId_ValidateCanonicalName(r->id);
            if (status != GEN3_RESOURCE_NAME_VALID)
            {
                Fail("canonical id invalid (%d): %s", (int)status, r->id);
                return false;
            }
            payloads[payloadCount].resource = r;
            Gen3ResourceId_DeriveKey(r->id, &payloads[payloadCount].key);
            payloadCount++;
        }
    }

    /* Duplicate ids and key collisions across ALL payload resources. */
    {
        size_t i;
        for (i = 0u; i < payloadCount; i++)
        {
            size_t j;
            for (j = 0u; j < i; j++)
            {
                if (strcmp(payloads[i].resource->id, payloads[j].resource->id) == 0)
                {
                    Fail("duplicate canonical id: %s",
                         payloads[i].resource->id);
                    return false;
                }
                if (Gen3ResourceId_KeyEqual(&payloads[i].key, &payloads[j].key))
                {
                    Fail("canonical key collision: %s and %s",
                         payloads[i].resource->id, payloads[j].resource->id);
                    return false;
                }
            }
        }
    }

    /* Alias ids must still be valid canonical names (they are never published,
     * but a malformed one would poison the source of truth). */
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            const struct PokemonResource *r = &families[k].resources[i];
            enum Gen3ResourceNameStatus status;
            if (!r->crossFamily && !r->externalAlias)
                continue;
            status = Gen3ResourceId_ValidateCanonicalName(r->id);
            if (status != GEN3_RESOURCE_NAME_VALID)
            {
                Fail("alias canonical id invalid (%d): %s",
                     (int)status, r->id);
                return false;
            }
        }
    }

    /* Per-family: slug uniqueness (a repeated slug would bind two resources
     * to the same source artifact), slot range/duplicate coverage, and the
     * exact 440-slot table shape. */
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        bool seen[SLOT_COUNT_PER_TABLE];
        size_t totalSlots = 0u;
        memset(seen, 0, sizeof(seen));

        for (i = 0u; i < families[k].resourceCount; i++)
        {
            const struct PokemonResource *a = &families[k].resources[i];
            size_t s;
            for (s = 0u; s < families[k].resourceCount; s++)
            {
                const struct PokemonResource *b = &families[k].resources[s];
                if (s == i)
                    continue;
                if (strcmp(a->slug, b->slug) == 0)
                {
                    Fail("duplicate slug '%s' in family '%s' (%s and %s)",
                         a->slug, kFamilies[k].kindName, a->id, b->id);
                    return false;
                }
            }
            if (a->slotCount == 0u)
            {
                Fail("resource has no slots: %s", a->id);
                return false;
            }
            for (s = 0u; s < a->slotCount; s++)
            {
                unsigned long long idx = (unsigned long long)a->slots[s].index;
                if (idx >= SLOT_COUNT_PER_TABLE)
                {
                    Fail("slot index out of range: %s[%llu]", a->id, idx);
                    return false;
                }
                if (seen[idx])
                {
                    Fail("duplicate slot index %llu in family '%s'", idx,
                         kFamilies[k].kindName);
                    return false;
                }
                seen[idx] = true;
                totalSlots++;
            }
        }
        if (totalSlots != SLOT_COUNT_PER_TABLE)
        {
            Fail("family '%s' covers %zu slots, expected %u",
                 kFamilies[k].kindName, totalSlots, SLOT_COUNT_PER_TABLE);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Emitters                                                            */
/* ------------------------------------------------------------------ */

struct AllResources
{
    struct PokemonResource *items;
    size_t count;
    size_t capacity;
};

static bool CollectPayloadResources(struct PokemonFamily *families,
                                    struct AllResources *all)
{
    enum Kind k;
    all->count = 0u;
    all->capacity = 0u;
    for (k = 0u; k < KIND_COUNT; k++)
        all->capacity += families[k].resourceCount;
    all->items = (struct PokemonResource *)calloc(all->capacity, sizeof(all->items[0]));
    if (all->items == NULL)
    {
        Fail("out of memory");
        return false;
    }
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            struct PokemonResource *r = &families[k].resources[i];
            if (r->crossFamily || r->externalAlias)
                continue;   /* aliases are not payloads */
            if (!DecodeArtifact(r))
                return false;
            memcpy(&all->items[all->count++], r, sizeof(all->items[0]));
        }
    }
    qsort(all->items, all->count, sizeof(all->items[0]), CompareResourceById);
    return true;
}

static bool EmitHeader(struct Gen3Buffer *out)
{
    return Gen3Buffer_AppendCStr(out,
        "# Generated by tools/gen3_resources/pokemon_family/gen_pokemon_family.\n"
        "# Do not edit by hand; edit the checked-in table headers / INCBIN\n"
        "# sources and re-run gen_pokemon_inventory.py then this generator.\n\n");
}

static bool EmitCatalog(struct Gen3Buffer *out, const struct AllResources *all)
{
    size_t i;
    Gen3Buffer_AppendCStr(out, "catalog_version = 1\n");
    Gen3TomlWrite_String(out, "namespace", POKEMON_NAMESPACE);
    Gen3TomlWrite_String(out, "resource_api", "1.0.0");
    Gen3TomlWrite_String(out, "game", "emerald");
    Gen3TomlWrite_String(out, "rom_profile", "bpee01-rev0");
    Gen3Buffer_AppendCStr(out, "\n");

    for (i = 0u; i < all->count; i++)
    {
        const struct PokemonResource *r = &all->items[i];
        Gen3TomlWrite_OpenArrayTable(out, "resources");
        Gen3TomlWrite_String(out, "id", r->id);
        Gen3TomlWrite_String(out, "type", r->kind == KIND_NORMAL_PALETTE
                                          || r->kind == KIND_SHINY_PALETTE
                                        ? POKEMON_TYPE_PALETTE : POKEMON_TYPE_SHEET);
        Gen3TomlWrite_Integer(out, "schema", POKEMON_SCHEMA);
        Gen3Buffer_AppendFormat(out, "required_for_base = %s\n", "true");
    }
    return true;
}

static bool EmitBindings(struct Gen3Buffer *out, const struct AllResources *all)
{
    size_t i;
    Gen3Buffer_AppendCStr(out,
        "\n# Semantic extraction bindings for the complete Pokémon battle-graphics\n"
        "# family. NO ROM offsets: they are derived at R9 time from the matching\n"
        "# GBA ELF symbol table via the R1A generator (gen3-elf-manifest), never\n"
        "# hand-maintained. Source encoding/representation mirror the catalog\n"
        "# types.\n\n");
    Gen3Buffer_AppendCStr(out, "bindings_version = 1\n");
    Gen3TomlWrite_String(out, "game", "emerald");
    Gen3TomlWrite_String(out, "rom_profile", "bpee01-rev0");
    Gen3Buffer_AppendCStr(out, "\n");

    for (i = 0u; i < all->count; i++)
    {
        const struct PokemonResource *r = &all->items[i];
        Gen3TomlWrite_OpenArrayTable(out, "bindings");
        Gen3TomlWrite_String(out, "id", r->id);
        Gen3TomlWrite_String(out, "symbol", r->symbol);
        Gen3TomlWrite_String(out, "source_artifact", r->artifact);
        Gen3TomlWrite_String(out, "source_encoding", r->encoding);
        Gen3TomlWrite_String(out, "canonical_representation", r->representation);
        Gen3TomlWrite_Integer(out, "expected_decoded_size", r->expectedDecodedSize);
    }
    return true;
}

static bool EmitOwnership(struct Gen3Buffer *out, const struct AllResources *all)
{
    size_t i;
    Gen3Buffer_AppendCStr(out,
        "\n# Native asset-ownership declaration. Each [[resources]] block records\n"
        "# the canonical id, M0/M1 key, legacy compiled symbol, committed source\n"
        "# artifact, per-target ownership state and source hashes.\n"
        "#   native = ROM_BASE_ONLY              -> payload NOT linked natively;\n"
        "#            served via ROM_BASE provider + EmeraldResourceCompat seam\n"
        "#            (the whole R9 family: all 1607 payloads are served from\n"
        "#            the ROM_BASE pack at runtime).\n"
        "#   gba    = COMPILED                  -> unchanged traditional build.\n"
        "# The two external aliases (gMonStillFrontPic_Egg, gMonPalette_Egg)\n"
        "# are NOT resources here: the back-EGG slot stays compiled on native\n"
        "# (still-front family); the shiny-EGG slot is served by the normal-\n"
        "# palette resource.\n"
        "# tests/run_emerald_native_asset_isolation.sh consumes this file and\n"
        "# FAILS when the native target contradicts it.\n\n");
    Gen3Buffer_AppendCStr(out, "ownership_version = 1\n");
    Gen3TomlWrite_String(out, "game", "emerald");
    Gen3TomlWrite_String(out, "rom_profile", "bpee01-rev0");
    Gen3Buffer_AppendCStr(out, "\n");

    for (i = 0u; i < all->count; i++)
    {
        const struct PokemonResource *r = &all->items[i];
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
        Gen3TomlWrite_String(out, "type", r->kind == KIND_NORMAL_PALETTE
                                          || r->kind == KIND_SHINY_PALETTE
                                        ? POKEMON_TYPE_PALETTE : POKEMON_TYPE_SHEET);
        Gen3TomlWrite_Integer(out, "schema", POKEMON_SCHEMA);
        Gen3TomlWrite_String(out, "source_artifact", r->artifact);
        Gen3TomlWrite_Integer(out, "encoded_length", (long long)r->encodedLength);
        Gen3TomlWrite_Integer(out, "decoded_length", (long long)r->decodedLength);
        Gen3TomlWrite_String(out, "source_encoding", r->encoding);
        Gen3Util_FormatHex(r->encodedSha, GEN3_RESOURCE_KEY_SIZE, encodedShaHex);
        Gen3TomlWrite_String(out, "source_encoded_sha256", encodedShaHex);
        Gen3Util_FormatHex(r->decodedSha, GEN3_RESOURCE_KEY_SIZE, decodedShaHex);
        Gen3TomlWrite_String(out, "canonical_decoded_sha256", decodedShaHex);
        Gen3TomlWrite_String(out, "ownership_state", "ROM_BASE_ONLY");
        Gen3Buffer_AppendCStr(out, "\n[resources.targets]\n");
        Gen3TomlWrite_String(out, "native", "ROM_BASE_ONLY");
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

static bool EmitConsumers(struct Gen3Buffer *out, struct PokemonFamily *families)
{
    struct TableSite sites[MAX_TABLE_SITES];
    size_t siteCount = 0u;
    enum Kind k;

    Gen3Buffer_AppendCStr(out,
        "# Machine-readable consumer map for the Pokémon battle-graphics\n"
        "# family (R9 §13).\n"
        "#   [[table_sites]]    one row per (table, GBA source location): the\n"
        "#                      checked-in sites that read the table generically\n"
        "#                      by species id. rom_header_gf.c rows are\n"
        "#                      layout-compat address embeds with no runtime\n"
        "#                      read (dead-site precedent, like R7A §6).\n"
        "#   [[resource_slots]] one row per (resource, table, index): every table\n"
        "#                      slot that consumes the resource. Multi-slot rows\n"
        "#                      are the aliases (DoubleQuestionMark x25,\n"
        "#                      Unown palettes x28, shiny-EGG -> normal palette).\n"
        "#   [[external_slots]] slots whose payload belongs to a family this\n"
        "#                      generator does not migrate: the back-table EGG\n"
        "#                      slot keeps its compiled gMonStillFrontPic_Egg\n"
        "#                      pointer on native; no canonical id exists.\n"
        "# No consumer is changed in R9.\n\n");
    Gen3Buffer_AppendCStr(out, "consumers_version = 1\n");
    Gen3TomlWrite_String(out, "game", "emerald");
    Gen3TomlWrite_String(out, "rom_profile", "bpee01-rev0");
    Gen3Buffer_AppendCStr(out, "\n");

    /* Per-table consumer sites (sorted bytewise by table, then site). */
    for (k = 0u; k < KIND_COUNT; k++)
    {
        struct SiteList list = SiteListForTable(kFamilies[k].table);
        size_t s;
        for (s = 0u; s < list.count; s++)
        {
            if (siteCount >= MAX_TABLE_SITES)
            {
                Fail("consumer-site table overflow");
                return false;
            }
            strcpy(sites[siteCount].table, kFamilies[k].table);
            strncpy(sites[siteCount].site, list.sites[s],
                    sizeof(sites[siteCount].site) - 1u);
            sites[siteCount].site[sizeof(sites[siteCount].site) - 1u] = '\0';
            siteCount++;
        }
    }
    qsort(sites, siteCount, sizeof(sites[0]), CompareTableSite);
    {
        size_t i;
        for (i = 0u; i < siteCount; i++)
        {
            Gen3TomlWrite_OpenArrayTable(out, "table_sites");
            Gen3TomlWrite_String(out, "table", sites[i].table);
            Gen3TomlWrite_String(out, "site", sites[i].site);
        }
    }
    Gen3Buffer_AppendCStr(out, "\n");

    /* Per-resource slots. families[k].resources are sorted bytewise by id
     * before this point (ValidateFamily callers sort); slot order per
     * resource follows the inventory (ascending index). */
    for (k = 0u; k < KIND_COUNT; k++)
    {
        size_t i;
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            const struct PokemonResource *r = &families[k].resources[i];
            const char *id = r->crossFamily ? r->aliasOf : r->id;
            size_t s;
            if (r->externalAlias)
                continue;   /* emitted below in external_slots */
            for (s = 0u; s < r->slotCount; s++)
            {
                Gen3TomlWrite_OpenArrayTable(out, "resource_slots");
                Gen3TomlWrite_String(out, "id", id);
                Gen3TomlWrite_String(out, "table", kFamilies[k].table);
                Gen3TomlWrite_Integer(out, "index", r->slots[s].index);
            }
        }
    }

    /* External alias slots (stays-compiled-on-native). */
    {
        size_t i;
        bool any = false;
        for (k = 0u; k < KIND_COUNT; k++)
        {
            for (i = 0u; i < families[k].resourceCount; i++)
            {
                const struct PokemonResource *r = &families[k].resources[i];
                size_t s;
                if (!r->externalAlias)
                    continue;
                for (s = 0u; s < r->slotCount; s++)
                {
                    Gen3TomlWrite_OpenArrayTable(out, "external_slots");
                    Gen3TomlWrite_String(out, "table", kFamilies[k].table);
                    Gen3TomlWrite_Integer(out, "index", r->slots[s].index);
                    Gen3TomlWrite_String(out, "symbol", r->symbol);
                    any = true;
                }
            }
        }
        (void)any;
    }
    return true;
}

static bool EmitSpeciesMapping(struct Gen3Buffer *out, struct PokemonFamily *families)
{
    enum Kind k;
    Gen3Buffer_AppendCStr(out,
        "# Total index mapping (R9 §4): every table slot, 0..439.\n"
        "# index == species id (designated initializer [SPECIES_##token]).\n"
        "# canonical = the payload-owning resource's id; alias slots resolve to\n"
        "# the same canonical as their payload (multi-slot rows); the external\n"
        "# back-EGG slot lists its compiled symbol instead (no canonical).\n\n");
    Gen3Buffer_AppendCStr(out, "species_mapping_version = 1\n");
    Gen3TomlWrite_String(out, "game", "emerald");
    Gen3TomlWrite_String(out, "rom_profile", "bpee01-rev0");
    Gen3Buffer_AppendCStr(out, "\n");

    for (k = 0u; k < KIND_COUNT; k++)
    {
        struct PokemonResource *byIndex[SLOT_COUNT_PER_TABLE];
        size_t i;
        long long idx;
        memset(byIndex, 0, sizeof(byIndex));

        for (i = 0u; i < families[k].resourceCount; i++)
        {
            struct PokemonResource *r = &families[k].resources[i];
            size_t s;
            for (s = 0u; s < r->slotCount; s++)
                byIndex[(size_t)r->slots[s].index] = r;
        }
        for (idx = 0; idx < (long long)SLOT_COUNT_PER_TABLE; idx++)
        {
            const struct PokemonResource *r = byIndex[(size_t)idx];
            const char *species = NULL;
            size_t s;
            Gen3TomlWrite_OpenArrayTable(out, "slots");
            Gen3TomlWrite_String(out, "table", kFamilies[k].table);
            Gen3TomlWrite_Integer(out, "index", idx);
            if (r == NULL)
            {
                Fail("slot %lld of '%s' uncovered", idx, kFamilies[k].table);
                return false;
            }
            for (s = 0u; s < r->slotCount; s++)
            {
                if (r->slots[s].index == idx)
                {
                    species = r->slots[s].species;
                    break;
                }
            }
            Gen3TomlWrite_String(out, "species", species != NULL ? species : "?");
            if (r->externalAlias)
            {
                Gen3TomlWrite_String(out, "external_symbol", r->symbol);
            }
            else
            {
                Gen3TomlWrite_String(out, "canonical",
                                     r->crossFamily ? r->aliasOf : r->id);
            }
        }
        Gen3Buffer_AppendCStr(out, "\n");
    }
    return true;
}

/* R9 §5: emit the native compat seam's generated C slot map (a header, not
 * TOML): the 1608 payload resources in bindings order (sorted by canonical
 * id, so image entry N == kPokemonBattleCompatResources[N] == bindings row N)
 * and the 1760 table slots in kind-major order (front 0..439, back 440..879,
 * normal palette 880..1319, shiny palette 1320..1759), each slot mapping to
 * its payload's resource index - or POKEMON_BATTLE_EXTERNAL_SLOT for the
 * external back-EGG slot, which stays compiled. The seam resolves and
 * publishes from exactly this table; there is no handwritten per-species
 * migration logic anywhere in the native publication path. */
static bool EmitCompatSlotMap(struct Gen3Buffer *out,
                              const struct AllResources *all,
                              struct PokemonFamily *families)
{
    enum Kind k;
    size_t i;
    bool *referenced;

    Gen3Buffer_AppendCStr(out,
        "/* Generated by tools/gen3_resources/pokemon_family/gen_pokemon_family.\n"
        " * Do not edit by hand; re-run the generator and --check it\n"
        " * (tests/gen3_resources/run_pokemon_family.sh).\n"
        " *\n"
        " * R9 §5 compat slot map for the native Pokémon battle tables.\n"
        " * kPokemonBattleCompatSlots is kind-major: slots 0..439 map into\n"
        " * gMonFrontPicTable, 440..879 gMonBackPicTable, 880..1319\n"
        " * gMonPaletteTable, 1320..1759 gMonShinyPaletteTable (slot index ==\n"
        " * species id, matching the designated initializers in\n"
        " * src/data/pokemon_graphics/). Each value is the index into\n"
        " * kPokemonBattleCompatResources of the payload-owning canonical the\n"
        " * slot publishes (multi-slot and cross-family rows share their\n"
        " * payload's index), or POKEMON_BATTLE_EXTERNAL_SLOT for the one slot\n"
        " * with no canonical (gMonBackPicTable[412], the EGG row that aliases\n"
        " * the compiled gMonStillFrontPic_Egg symbol). Resource order matches\n"
        " * bindings.generated.toml (sorted by canonical id). */\n");
    Gen3Buffer_AppendCStr(out, "#ifndef EMERALD_RESOURCES_POKEMON_BATTLE_SLOTS_GENERATED_H\n");
    Gen3Buffer_AppendCStr(out, "#define EMERALD_RESOURCES_POKEMON_BATTLE_SLOTS_GENERATED_H\n\n");
    Gen3Buffer_AppendCStr(out, "#include <stdint.h>\n");
    Gen3Buffer_AppendCStr(out, "#include \"gen3/resources/resource_types.h\"\n\n");
    Gen3Buffer_AppendFormat(out,
        "#define POKEMON_BATTLE_SLOTS_PER_TABLE %lluu\n",
        (unsigned long long)SLOT_COUNT_PER_TABLE);
    Gen3Buffer_AppendFormat(out,
        "#define POKEMON_BATTLE_SLOT_COUNT %lluu\n",
        (unsigned long long)KIND_COUNT * SLOT_COUNT_PER_TABLE);
    Gen3Buffer_AppendFormat(out,
        "#define POKEMON_BATTLE_RESOURCE_COUNT %zu\n\n", all->count);
    Gen3Buffer_AppendCStr(out,
        "#define POKEMON_BATTLE_EXTERNAL_SLOT (-1)\n\n"
        "struct PokemonBattleCompatResource\n"
        "{\n"
        "    const char *id;             /* canonical id, e.g. \"emerald:pokemon/bulbasaur/battle/front/sheet\" */\n"
        "    uint32_t expectedSize;      /* decoded size (the GBA LZ77 header's declared length) */\n"
        "    enum Gen3ResourceType type; /* TILE_GRAPHICS (sheet) or PALETTE */\n"
        "    uint32_t schema;            /* 1 */\n"
        "};\n\n");

    Gen3Buffer_AppendFormat(out,
        "static const struct PokemonBattleCompatResource\n"
        "    kPokemonBattleCompatResources[POKEMON_BATTLE_RESOURCE_COUNT] =\n"
        "{\n");
    for (i = 0u; i < all->count; i++)
    {
        const struct PokemonResource *r = &all->items[i];
        const char *typeToken = (r->kind == KIND_NORMAL_PALETTE
                                 || r->kind == KIND_SHINY_PALETTE)
            ? "PALETTE" : "TILE_GRAPHICS";
        Gen3Buffer_AppendFormat(out,
            "    { \"%s\", %lluu, GEN3_RESOURCE_TYPE_%s, %lluu },\n",
            r->id, (unsigned long long)r->expectedDecodedSize, typeToken,
            (unsigned long long)POKEMON_SCHEMA);
    }
    Gen3Buffer_AppendCStr(out, "};\n\n");

    referenced = (bool *)calloc(all->count, sizeof(referenced[0]));
    if (referenced == NULL)
    {
        Fail("out of memory");
        return false;
    }
    Gen3Buffer_AppendFormat(out,
        "static const int32_t kPokemonBattleCompatSlots[POKEMON_BATTLE_SLOT_COUNT] =\n"
        "{\n");
    for (k = 0u; k < KIND_COUNT; k++)
    {
        struct PokemonResource *byIndex[SLOT_COUNT_PER_TABLE];
        long long idx;
        memset(byIndex, 0, sizeof(byIndex));
        for (i = 0u; i < families[k].resourceCount; i++)
        {
            struct PokemonResource *r = &families[k].resources[i];
            size_t s;
            for (s = 0u; s < r->slotCount; s++)
                byIndex[(size_t)r->slots[s].index] = r;
        }
        Gen3Buffer_AppendFormat(out, "    /* %s (slots %lld..%lld) */\n",
                                kFamilies[k].table,
                                (long long)k * (long long)SLOT_COUNT_PER_TABLE,
                                (long long)(k + 1) * (long long)SLOT_COUNT_PER_TABLE - 1);
        for (idx = 0; idx < (long long)SLOT_COUNT_PER_TABLE; idx++)
        {
            const struct PokemonResource *r = byIndex[(size_t)idx];
            int emitted = 0;
            if (r == NULL)
            {
                Fail("slot %lld of '%s' uncovered", idx, kFamilies[k].table);
                free(referenced);
                return false;
            }
            if (r->externalAlias)
            {
                /* No canonical: the slot's compiled symbol stays live on
                 * native (the back-EGG slot gMonStillFrontPic_Egg). */
                Gen3Buffer_AppendCStr(out, "    POKEMON_BATTLE_EXTERNAL_SLOT,\n");
                continue;
            }
            {
                const char *payloadId = r->crossFamily ? r->aliasOf : r->id;
                size_t p;
                for (p = 0u; p < all->count; p++)
                {
                    if (strcmp(all->items[p].id, payloadId) == 0)
                    {
                        Gen3Buffer_AppendFormat(out, "    %zu,\n", p);
                        referenced[p] = true;
                        emitted = 1;
                        break;
                    }
                }
                if (!emitted)
                {
                    Fail("slot %lld of '%s' resolves to '%s', which is not a "
                         "payload resource", idx, kFamilies[k].table, payloadId);
                    free(referenced);
                    return false;
                }
            }
        }
    }
    Gen3Buffer_AppendCStr(out, "};\n\n#endif\n");
    for (i = 0u; i < all->count; i++)
    {
        if (!referenced[i])
        {
            Fail("payload resource '%s' is referenced by no table slot",
                 all->items[i].id);
            free(referenced);
            return false;
        }
    }
    free(referenced);
    return true;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void PrintUsage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --inventory PATH\n"
        "       --catalog-out PATH --bindings-out PATH\n"
        "       --ownership-out PATH --consumers-out PATH\n"
        "       --species-map-out PATH --compat-slotmap-out PATH\n"
        "       [--check]\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *inventoryPath = NULL;
    const char *catalogOut = NULL;
    const char *bindingsOut = NULL;
    const char *ownershipOut = NULL;
    const char *consumersOut = NULL;
    const char *speciesMapOut = NULL;
    const char *compatSlotmapOut = NULL;
    int doCheck = 0;
    int i;
    struct Gen3Buffer text;
    struct Gen3TomlDocument doc;
    struct PokemonFamily families[KIND_COUNT];
    struct AllResources all;
    struct Gen3Buffer catalog;
    struct Gen3Buffer bindings;
    struct Gen3Buffer ownership;
    struct Gen3Buffer consumers;
    struct Gen3Buffer speciesMap;
    struct Gen3Buffer compatSlotmap;
    char errbuf[512];
    enum Kind k;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--inventory") == 0 && i + 1 < argc)
            inventoryPath = argv[++i];
        else if (strcmp(argv[i], "--catalog-out") == 0 && i + 1 < argc)
            catalogOut = argv[++i];
        else if (strcmp(argv[i], "--bindings-out") == 0 && i + 1 < argc)
            bindingsOut = argv[++i];
        else if (strcmp(argv[i], "--ownership-out") == 0 && i + 1 < argc)
            ownershipOut = argv[++i];
        else if (strcmp(argv[i], "--consumers-out") == 0 && i + 1 < argc)
            consumersOut = argv[++i];
        else if (strcmp(argv[i], "--species-map-out") == 0 && i + 1 < argc)
            speciesMapOut = argv[++i];
        else if (strcmp(argv[i], "--compat-slotmap-out") == 0 && i + 1 < argc)
            compatSlotmapOut = argv[++i];
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

    if (inventoryPath == NULL)
    {
        Fail("--inventory is required");
        return 1;
    }
    if (!doCheck && (catalogOut == NULL || bindingsOut == NULL
                     || ownershipOut == NULL || consumersOut == NULL
                     || speciesMapOut == NULL || compatSlotmapOut == NULL))
    {
        Fail("all six --*-out paths are required (or use --check)");
        return 1;
    }

    memset(families, 0, sizeof(families));
    for (k = 0u; k < KIND_COUNT; k++)
    {
        families[k] = kFamilies[k];
        families[k].resourceCount = 0u;
    }

    if (!Gen3Util_ReadFile(inventoryPath, &text, errbuf, sizeof(errbuf)))
    {
        Fail("cannot read inventory: %s", errbuf);
        Gen3Buffer_Destroy(&text);
        return gExitCode ? gExitCode : 1;
    }
    if (!Gen3Toml_Parse(text.data, text.length, &doc, errbuf, sizeof(errbuf)))
    {
        Fail("malformed inventory: %s", errbuf);
        Gen3Buffer_Destroy(&text);
        return gExitCode ? gExitCode : 1;
    }
    {
        long long familiesCount = (long long)Gen3Toml_GetArrayCount(&doc.root, "families");
        if (familiesCount != (long long)KIND_COUNT)
        {
            Fail("inventory declares %lld families, expected %d",
                 familiesCount, (int)KIND_COUNT);
            Gen3Toml_Destroy(&doc);
            Gen3Buffer_Destroy(&text);
            return gExitCode ? gExitCode : 1;
        }
    }
    if (!ParseResources(&doc, families)
     || !ParseSlots(&doc, families)
     || !ValidateSlotJoins(families)
     || !ResolveAliases(families)
     || !ValidateFamily(families))
    {
        Gen3Toml_Destroy(&doc);
        Gen3Buffer_Destroy(&text);
        return gExitCode ? gExitCode : 1;
    }
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&text);

    if (!CollectPayloadResources(families, &all))
    {
        free(all.items);
        return gExitCode ? gExitCode : 1;
    }

    /* Sort each family's resources by id (payload + aliases) so the consumers
     * and species-map emitters are deterministic too. */
    for (k = 0u; k < KIND_COUNT; k++)
    {
        qsort(families[k].resources, families[k].resourceCount,
              sizeof(families[k].resources[0]), CompareResourceById);
    }

    Gen3Buffer_Init(&catalog, 1u << 20);
    Gen3Buffer_Init(&bindings, 1u << 20);
    Gen3Buffer_Init(&ownership, 1u << 20);
    Gen3Buffer_Init(&consumers, 1u << 20);
    Gen3Buffer_Init(&speciesMap, 1u << 20);
    Gen3Buffer_Init(&compatSlotmap, 1u << 20);
    EmitHeader(&catalog);
    EmitHeader(&bindings);
    EmitHeader(&ownership);
    EmitHeader(&consumers);
    EmitHeader(&speciesMap);
    /* The compat slot map is a C header, not a TOML document: EmitHeader's
     * '#' banner is for the TOML outputs only; EmitCompatSlotMap emits its
     * own C banner. */
    if (!EmitCatalog(&catalog, &all)
     || !EmitBindings(&bindings, &all)
     || !EmitOwnership(&ownership, &all)
     || !EmitConsumers(&consumers, families)
     || !EmitSpeciesMapping(&speciesMap, families)
     || !EmitCompatSlotMap(&compatSlotmap, &all, families))
    {
        Gen3Buffer_Destroy(&catalog);
        Gen3Buffer_Destroy(&bindings);
        Gen3Buffer_Destroy(&ownership);
        Gen3Buffer_Destroy(&consumers);
        Gen3Buffer_Destroy(&speciesMap);
        Gen3Buffer_Destroy(&compatSlotmap);
        free(all.items);
        return gExitCode ? gExitCode : 1;
    }

    if (doCheck)
    {
        int mismatch = 0;
        struct
        {
            const char *path;
            struct Gen3Buffer *buf;
        } targets[6] = {
            { catalogOut, &catalog },
            { bindingsOut, &bindings },
            { ownershipOut, &ownership },
            { consumersOut, &consumers },
            { speciesMapOut, &speciesMap },
            { compatSlotmapOut, &compatSlotmap },
        };
        int t;
        for (t = 0; t < 6; t++)
        {
            struct Gen3Buffer existing; /* ReadFile owns init */
            if (targets[t].path == NULL)
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
        Gen3Buffer_Destroy(&catalog);
        Gen3Buffer_Destroy(&bindings);
        Gen3Buffer_Destroy(&ownership);
        Gen3Buffer_Destroy(&consumers);
        Gen3Buffer_Destroy(&speciesMap);
        Gen3Buffer_Destroy(&compatSlotmap);
        free(all.items);
        if (!mismatch)
            fprintf(stderr, "check passed: %zu payload resources, %zu byte total output\n",
                    all.count,
                    catalog.length + bindings.length + ownership.length
                    + consumers.length + speciesMap.length
                    + compatSlotmap.length);
        return mismatch ? 1 : 0;
    }

    if (!Gen3Util_WriteFile(catalogOut, catalog.data, catalog.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(bindingsOut, bindings.data, bindings.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(ownershipOut, ownership.data, ownership.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(consumersOut, consumers.data, consumers.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(speciesMapOut, speciesMap.data, speciesMap.length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(compatSlotmapOut, compatSlotmap.data,
                            compatSlotmap.length, errbuf, sizeof(errbuf)))
    {
        Fail("cannot write outputs: %s", errbuf);
        Gen3Buffer_Destroy(&catalog);
        Gen3Buffer_Destroy(&bindings);
        Gen3Buffer_Destroy(&ownership);
        Gen3Buffer_Destroy(&consumers);
        Gen3Buffer_Destroy(&speciesMap);
        Gen3Buffer_Destroy(&compatSlotmap);
        free(all.items);
        return 1;
    }

    {
        size_t perFamily[KIND_COUNT];
        for (k = 0u; k < KIND_COUNT; k++)
        {
            size_t i;
            perFamily[k] = 0u;
            for (i = 0u; i < families[k].resourceCount; i++)
            {
                if (!families[k].resources[i].crossFamily
                 && !families[k].resources[i].externalAlias)
                    perFamily[k]++;
            }
        }
        fprintf(stderr, "wrote %zu payload resources (front %zu, back %zu, "
                        "palette %zu, shiny %zu):\n"
                        "  %s\n  %s\n  %s\n  %s\n  %s\n",
                all.count,
                perFamily[KIND_FRONT_SHEET], perFamily[KIND_BACK_SHEET],
                perFamily[KIND_NORMAL_PALETTE], perFamily[KIND_SHINY_PALETTE],
                catalogOut, bindingsOut, ownershipOut, consumersOut,
                speciesMapOut);
    }

    Gen3Buffer_Destroy(&catalog);
    Gen3Buffer_Destroy(&bindings);
    Gen3Buffer_Destroy(&ownership);
    Gen3Buffer_Destroy(&consumers);
    Gen3Buffer_Destroy(&speciesMap);
    free(all.items);
    return 0;
}
