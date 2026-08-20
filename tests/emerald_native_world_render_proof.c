/* R11-F: Harness D — the renderer-facing proof (plan §11 Harness D,
 * lines 557-567). Real pack + real data (Harness B link set, see
 * run_emerald_native_world_render_proof.sh).
 *
 * Offscreen tilemap-entry render: NO pixels, NO SDL, NO viewport code.
 * The rect covers world blocks x in [0,20), y in [-10,10) — 20x20 blocks
 * = 320x320 px, exceeding the 240x160 presentation view in BOTH
 * dimensions (assertion i) — and crosses the real Littleroot<->Route101
 * north connection at world y = 0: rows y < 0 are Route101 (neighbor),
 * rows y >= 0 are Littleroot (current).
 *
 * For every block: NativeWorldNeighborhood_ResolveBlockForRender yields
 * the source + owner map identity + local coords + owner layout + metatile
 * id + layer type (the DrawMetatileAt contract, field_camera.c:238-255);
 * the metatile's 8 tile words are read from the OWNER layout's metatile
 * tables with DrawMetatileAt's id < 512 primary / else secondary split,
 * and the 4 quadrant entries per BG (BG1/BG2/BG3) are written via
 * NativeWorldNeighborhood_MetatileTileEntryForLayer into three 20x20x4
 * buffers (4800 entries).
 *
 * The buffers must byte-match an INDEPENDENT oracle that reimplements the
 * resolution by hand directly from the two layouts' published
 * .map/.border + metatile tables (the two-independent-derivations
 * pattern): the oracle reads the same canonical data through the published
 * accessor, but its resolution logic is written from field_camera.c's
 * DrawMetatileAt/DrawMetatile, NOT from the module.
 *
 * NOTE on the plan's size parenthetical (report §14): the plan's "480x160
 * px at 8 px/tile, 60x20 tiles" is inconsistent with its own explicit
 * block rect x in [0,20), y in [-10,10) (20x20 blocks = 320x320 px = 40x40
 * tiles per BG) — and with its own assertion (iv), which requires rows on
 * BOTH sides of y = 0 inside the rect. The rect as stated satisfies every
 * assertion: (i) holds in both dimensions (320 > 240 AND 320 > 160).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "fieldmap.h"
#include "overworld.h"
#include "constants/map_groups.h"
#include "platform/native_world_neighborhood.h"
#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_trainer_native_compat.h"

static unsigned sChecks;
static unsigned sFailures;

#define CHECK(cond)                                                     \
    do {                                                                \
        sChecks++;                                                      \
        if (!(cond)) {                                                  \
            sFailures++;                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b) CHECK((a) == (b))

/* ------------------------------------------------------------ data pins */

/* map_groups.json identities used by the render proof (group order ==
 * gMapGroups table order; mapNum = index within the group's list). */
#define MAP_GROUP_TOWNS_AND_ROUTES 0
#define MAP_NUM_LITTLEROOT_TOWN 9
#define MAP_NUM_ROUTE101 16

/* -------------------------------------------------------------- helpers */

static void SetLocation(u8 group, u8 num)
{
    /* The `struct SaveBlock1` fixture lives in the overworld stub TU. */
    gSaveBlock1Ptr->location.mapGroup = group;
    gSaveBlock1Ptr->location.mapNum = num;
}

static const struct MapHeader *HeaderFor(u8 group, u8 num)
{
    return Overworld_GetMapHeaderByGroupAndId(group, num);
}

/* maps.o's header records carry GBA-era 2-byte alignment while the native
 * struct MapHeader requires 8 (UBSan note, baseline noise — the module's
 * own reads emit it too). Copy the record alignment-safely so this
 * harness adds none of its own. */
static struct MapHeader HeaderCopy(u8 group, u8 num)
{
    struct MapHeader hdr;
    memcpy(&hdr, HeaderFor(group, num), sizeof(hdr));
    return hdr;
}

/* ------------------------------------------------------ the offscreen rect */

#define RENDER_BLOCKS_W 20     /* world blocks x in [0, 20) */
#define RENDER_BLOCKS_H 20     /* world blocks y in [-10, 10) */
#define RENDER_MIN_Y (-10)
#define RENDER_NUM_BG 3        /* BG1/BG2/BG3 (the seam's bg = 1/2/3) */

/* Per BG: [block row][block col][quadrant]; quadrant = (tileY & 1) * 2 +
 * (tileX & 1) — the renderer's GetMetatileTileEntryFromSnapshot order
 * (TL, TR, BL, BR). Three 20x20x4 buffers = 4800 tile entries. */
static u16 sTilemap[RENDER_NUM_BG][RENDER_BLOCKS_H][RENDER_BLOCKS_W][4];
static u16 sOracle[RENDER_NUM_BG][RENDER_BLOCKS_H][RENDER_BLOCKS_W][4];

/* DrawMetatileAt's metatile-tile lookup (field_camera.c:243-253): clamp,
 * then id < 512 primary / else secondary minus 512, 8 tile words per
 * metatile (4 bottom + 4 top). */
static const u16 *MetatileTilesForId(const struct MapLayout *layout, u16 metatileId)
{
    if (metatileId < NUM_METATILES_IN_PRIMARY)
        return layout->primaryTileset->metatiles + metatileId * NUM_TILES_PER_METATILE;
    return layout->secondaryTileset->metatiles
         + (metatileId - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE;
}

/* ------------------------------------------------- independent oracle */

/* Reimplements field_camera.c's DrawMetatile (lines 257-303) at the
 * single-entry granularity of the seam's MetatileTileEntryForLayer
 * contract: SPLIT -> bottom on BG3 + top on BG1 (BG2 transparent),
 * COVERED -> bottom on BG3 + top on BG2 (BG1 transparent), NORMAL ->
 * bottom on BG2 + top on BG1 (BG3 = the 0x3014 garbage tile). Written
 * from field_camera.c, not from the module. */
static u16 OracleEntry(u8 bg, const u16 *tiles, u8 layerType, u8 quadrant)
{
    switch (layerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        if (bg == 3)
            return tiles[quadrant];
        if (bg == 1)
            return tiles[quadrant + 4];
        return 0;
    case METATILE_LAYER_TYPE_COVERED:
        if (bg == 3)
            return tiles[quadrant];
        if (bg == 2)
            return tiles[quadrant + 4];
        return 0;
    case METATILE_LAYER_TYPE_NORMAL:
    default:
        if (bg == 3)
            return 0x3014;
        if (bg == 2)
            return tiles[quadrant];
        if (bg == 1)
            return tiles[quadrant + 4];
        return 0;
    }
}

/* Reimplements the block resolution by hand from the two layouts'
 * published .map + metatile tables: world x in [0,20) and y in [0,20)
 * is Littleroot (current); world x in [0,20) and y in [-20,0) is
 * Route101 (north neighbor at world (0,-20), the NORTH formula
 * (offset, -cHeight)). Any cell outside that union is a failure — the
 * rect must never touch BORDER (assertion iv). */
static void BuildOracle(const struct MapLayout *litLayout, const struct MapLayout *r101Layout)
{
    int by;
    int bx;
    for (by = 0; by < RENDER_BLOCKS_H; by++)
    {
        for (bx = 0; bx < RENDER_BLOCKS_W; bx++)
        {
            s32 wx = bx;
            s32 wy = RENDER_MIN_Y + by;
            const struct MapLayout *owner;
            u16 block;
            u16 id;
            u8 layerType;
            const u16 *tiles;
            int bg;
            int q;

            if (wx >= 0 && wx < litLayout->width
             && wy >= 0 && wy < litLayout->height)
            {
                owner = litLayout;
                block = litLayout->map[wy * litLayout->width + wx];
            }
            else if (wx >= 0 && wx < r101Layout->width
                  && wy >= -r101Layout->height && wy < 0)
            {
                owner = r101Layout;
                block = r101Layout->map[(wy + r101Layout->height) * r101Layout->width + wx];
            }
            else
            {
                fprintf(stderr, "oracle: cell (%d,%d) outside current+neighbor union\n",
                        wx, wy);
                exit(1);
            }

            id = block & MAPGRID_METATILE_ID_MASK;
            if (id > NUM_METATILES_TOTAL)
                id = 0; /* DrawMetatileAt's clamp (field_camera.c:243-244) */
            if (id < NUM_METATILES_IN_PRIMARY)
            {
                tiles = owner->primaryTileset->metatiles + id * NUM_TILES_PER_METATILE;
                layerType = UNPACK_LAYER_TYPE(owner->primaryTileset->metatileAttributes[id]);
            }
            else
            {
                tiles = owner->secondaryTileset->metatiles
                      + (id - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE;
                layerType = UNPACK_LAYER_TYPE(
                    owner->secondaryTileset->metatileAttributes[id - NUM_METATILES_IN_PRIMARY]);
            }

            for (bg = 1; bg <= 3; bg++)
            {
                for (q = 0; q < 4; q++)
                    sOracle[bg - 1][by][bx][q] = OracleEntry((u8)bg, tiles, layerType, (u8)q);
            }
        }
    }
}

/* ------------------------------------------------------- the render proof */

static void TestRenderProof(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct MapLayout *litLayout;
    const struct MapLayout *r101Layout;
    enum NativeWorldBlockSource rowSource[RENDER_BLOCKS_H];
    int flipCount = 0;
    int by;
    int bx;

    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    CHECK(nb != NULL);
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 1); /* the single north Route101 record */

    litLayout = HeaderCopy(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN).mapLayout;
    r101Layout = HeaderCopy(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE101).mapLayout;
    CHECK(litLayout != NULL);
    CHECK(r101Layout != NULL);
    CHECK_EQ_INT(litLayout->width, RENDER_BLOCKS_W);
    CHECK_EQ_INT(litLayout->height, RENDER_BLOCKS_H);
    CHECK_EQ_INT(r101Layout->width, RENDER_BLOCKS_W);
    CHECK_EQ_INT(r101Layout->height, RENDER_BLOCKS_H);

    /* (ii) the layouts' blockdata + tilesets must be PUBLISHED (real
     * .map/.border + metatile tables from the pack) — the oracle reads
     * them directly. */
    CHECK(litLayout->map != NULL);
    CHECK(litLayout->border != NULL);
    CHECK(litLayout->primaryTileset != NULL);
    CHECK(litLayout->primaryTileset->metatiles != NULL);
    CHECK(litLayout->primaryTileset->metatileAttributes != NULL);
    CHECK(litLayout->secondaryTileset != NULL);
    CHECK(litLayout->secondaryTileset->metatiles != NULL);
    CHECK(litLayout->secondaryTileset->metatileAttributes != NULL);
    CHECK(r101Layout->map != NULL);
    CHECK(r101Layout->border != NULL);
    CHECK(r101Layout->primaryTileset != NULL);
    CHECK(r101Layout->primaryTileset->metatiles != NULL);
    CHECK(r101Layout->primaryTileset->metatileAttributes != NULL);
    CHECK(r101Layout->secondaryTileset != NULL);
    CHECK(r101Layout->secondaryTileset->metatiles != NULL);
    CHECK(r101Layout->secondaryTileset->metatileAttributes != NULL);

    /* (ii) the two blockdata arrays genuinely differ — the byte-match
     * against the oracle is a real cross-check, not two reads of the
     * same bytes. */
    CHECK(memcmp(litLayout->map, r101Layout->map,
                 (size_t)litLayout->width * litLayout->height * sizeof(u16)) != 0);

    /* (i) the rect exceeds the 240x160 presentation view in BOTH
     * dimensions (320x320 px). */
    CHECK(RENDER_BLOCKS_W * 16 > 240);
    CHECK(RENDER_BLOCKS_H * 16 > 160);

    for (by = 0; by < RENDER_BLOCKS_H; by++)
    {
        rowSource[by] = NATIVE_WORLD_BLOCK_CURRENT; /* never observed unset */
        for (bx = 0; bx < RENDER_BLOCKS_W; bx++)
        {
            s32 wx = bx;
            s32 wy = RENDER_MIN_Y + by;
            enum NativeWorldBlockSource src;
            u8 mg;
            u8 mn;
            u8 layerType;
            s32 lx;
            s32 ly;
            const struct MapLayout *ownerLayout;
            u16 metatileId;
            const u16 *tiles;
            int bg;
            int q;

            CHECK(NativeWorldNeighborhood_ResolveBlockForRender(nb, wx, wy, &src, &mg, &mn,
                                                                &lx, &ly, &ownerLayout,
                                                                &metatileId, &layerType));
            if (by < RENDER_BLOCKS_H / 2)
            {
                /* (ii)/(iv) rows y < 0: Route101's blockdata, Route101's
                 * tilesets, world y maps to local y + 20. */
                CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
                CHECK_EQ_INT(mg, MAP_GROUP_TOWNS_AND_ROUTES);
                CHECK_EQ_INT(mn, MAP_NUM_ROUTE101);
                CHECK(ownerLayout == r101Layout);
                CHECK_EQ_INT(lx, wx);
                CHECK_EQ_INT(ly, wy + r101Layout->height);
            }
            else
            {
                /* (ii)/(iv) rows y >= 0: Littleroot's blockdata, Littleroot's
                 * tilesets. */
                CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
                CHECK_EQ_INT(mg, MAP_GROUP_TOWNS_AND_ROUTES);
                CHECK_EQ_INT(mn, MAP_NUM_LITTLEROOT_TOWN);
                CHECK(ownerLayout == litLayout);
                CHECK_EQ_INT(lx, wx);
                CHECK_EQ_INT(ly, wy);
            }
            CHECK(src != NATIVE_WORLD_BLOCK_BORDER); /* (iv) no BORDER in the rect */
            if (bx == 0)
                rowSource[by] = src;
            else
                CHECK_EQ_INT(src, rowSource[by]); /* a row is wholly one map */

            /* The seam contract: the renderer reads the metatile's 8 tile
             * words from the OWNER layout with DrawMetatileAt's split. */
            tiles = MetatileTilesForId(ownerLayout, metatileId);

            for (bg = 1; bg <= 3; bg++)
            {
                for (q = 0; q < 4; q++)
                    sTilemap[bg - 1][by][bx][q] =
                        NativeWorldNeighborhood_MetatileTileEntryForLayer((u8)bg, tiles,
                                                                          layerType, (u8)q);
            }
        }
    }

    /* (iv) the source flips exactly once, between rows y = -1 and y = 0. */
    CHECK_EQ_INT(rowSource[0], NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(rowSource[RENDER_BLOCKS_H - 1], NATIVE_WORLD_BLOCK_CURRENT);
    for (by = 1; by < RENDER_BLOCKS_H; by++)
    {
        if (rowSource[by] != rowSource[by - 1])
            flipCount++;
    }
    CHECK_EQ_INT(flipCount, 1);

    /* (iv) BORDER is a live value — control cells outside the
     * current+neighbor union must resolve to it (so "BORDER nowhere
     * inside the rect" is a real constraint). West of Littleroot and
     * north of Route101 have no connections: both are BORDER. */
    {
        enum NativeWorldBlockSource src;
        u8 mg;
        u8 mn;
        s32 lx;
        s32 ly;
        const struct MapLayout *ownerLayout;
        u16 metatileId;
        u8 layerType;

        CHECK(NativeWorldNeighborhood_ResolveBlockForRender(nb, -5, 5, &src, &mg, &mn,
                                                            &lx, &ly, &ownerLayout,
                                                            &metatileId, &layerType));
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);
        CHECK_EQ_INT(mg, MAP_GROUP_TOWNS_AND_ROUTES);
        CHECK_EQ_INT(mn, MAP_NUM_LITTLEROOT_TOWN); /* border belongs to the current map */

        CHECK(NativeWorldNeighborhood_ResolveBlockForRender(nb, 0, -30, &src, &mg, &mn,
                                                            &lx, &ly, &ownerLayout,
                                                            &metatileId, &layerType));
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);
    }

    /* (iii) the buffers byte-match the independent oracle. */
    BuildOracle(litLayout, r101Layout);
    CHECK(memcmp(sTilemap, sOracle, sizeof(sTilemap)) == 0);
}

/* ------------------------------------------------------- session drive */

static enum Gen3ResourceType CatalogTypeForName(const char *name)
{
    /* Same vocabulary as the importer's ParseTypeName (the catalog files are
     * generated by the same family generators, so the test loader must accept
     * every type the generated catalogs emit - tileset/tilemap included). */
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
    if (strcmp(name, "instrument-bank") == 0)
        return GEN3_RESOURCE_TYPE_INSTRUMENT_BANK;
    if (strcmp(name, "structured-data") == 0)
        return GEN3_RESOURCE_TYPE_STRUCTURED_DATA;
    return GEN3_RESOURCE_TYPE_INVALID;
}

static bool AddCatalogFile(struct Gen3ResourceCatalog *catalog, const char *path)
{
    struct Gen3Buffer buf;
    struct Gen3TomlDocument doc;
    struct Gen3ResourceDiagnosticList diag;
    char errbuf[512];
    size_t i;
    bool ok = false;

    if (!Gen3Util_ReadFile(path, &buf, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "catalog read: %s\n", errbuf);
        return false;
    }
    if (!Gen3Toml_Parse(buf.data, buf.length, &doc, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "catalog parse: %s\n", errbuf);
        Gen3Buffer_Destroy(&buf);
        return false;
    }
    Gen3ResourceDiagnostics_Init(&diag);
    for (i = 0; i < Gen3Toml_GetArrayCount(&doc.root, "resources"); i++)
    {
        const struct Gen3TomlMap *item =
            Gen3Toml_GetArrayItem(&doc.root, "resources", i);
        const char *id, *type;
        long long schema;
        bool required = false;
        if (!Gen3Toml_GetString(item, "id", &id)
         || !Gen3Toml_GetString(item, "type", &type)
         || !Gen3Toml_GetInteger(item, "schema", &schema))
        {
            fprintf(stderr, "catalog record %zu missing id/type/schema\n", i);
            goto done;
        }
        Gen3Toml_GetBool(item, "required_for_base", &required);
        if (!Gen3ResourceCatalog_Add(catalog, id, CatalogTypeForName(type),
                                     (uint32_t)schema, required, &diag))
        {
            fprintf(stderr, "catalog add failed for %s\n", id);
            goto done;
        }
    }
    ok = true;
done:
    Gen3ResourceDiagnostics_Destroy(&diag);
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&buf);
    return ok;
}

int main(int argc, char **argv)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList gdiag;
    struct EmeraldResourceSessionInfo info;
    struct EmeraldResourceCompatDiagnostics cdiag;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    printf("== R11-F Harness D: offscreen render proof, real data + "
           "production pack ==\n");

    {
        const char *catalogPaths[16];
        size_t catalogCount = 0u;
        int arg;
        for (arg = 2; arg < argc; arg++)
        {
            if (strcmp(argv[arg], "--catalog") == 0 && arg + 1 < argc
             && catalogCount < sizeof(catalogPaths) / sizeof(catalogPaths[0]))
            {
                catalogPaths[catalogCount++] = argv[++arg];
            }
            else
            {
                fprintf(stderr, "usage: %s <pack.rpack> --catalog <catalog.toml>...\n",
                        argv[0]);
                return 2;
            }
        }
        if (catalogCount == 0u)
        {
            fprintf(stderr, "usage: %s <pack.rpack> --catalog <catalog.toml>...\n",
                    argv[0]);
            return 2;
        }
        catalog = Gen3ResourceCatalog_Create();
        if (catalog == NULL)
        {
            fprintf(stderr, "out of memory creating catalog\n");
            return 1;
        }
        for (arg = 0; (size_t)arg < catalogCount; arg++)
        {
            if (!AddCatalogFile(catalog, catalogPaths[arg]))
            {
                Gen3ResourceCatalog_Destroy(catalog);
                return 1;
            }
        }
        Gen3ResourceDiagnostics_Init(&gdiag);
        if (!Gen3ResourceCatalog_Finalize(catalog, &gdiag))
        {
            fprintf(stderr, "catalog finalize failed\n");
            Gen3ResourceDiagnostics_Destroy(&gdiag);
            Gen3ResourceCatalog_Destroy(catalog);
            return 1;
        }
        Gen3ResourceDiagnostics_Destroy(&gdiag);
    }

    /* 1. Open the REAL installed production pack. */
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(argv[1], &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
    {
        printf("FAIL: cannot open pack '%s'\n", argv[1]);
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        Gen3ResourceCatalog_Destroy(catalog);
        return 1;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    CHECK(Gen3ResourcePack_GetEntryCount(pack) == 15750u); /* 4518 + 569 audio leaves + 202 structural + 530 song graphs (R12-B/C/D) + 1057 leaf (R13-B) + 5187 text (R13-C) + 3310 D1 */
    CHECK(Gen3ResourceCatalog_Count(catalog) == 15750u); /* + 569 audio leaves + 202 structural + 530 song graphs (R12-B/C/D) + 1057 leaf (R13-B) + 5187 text (R13-C) + 3310 D1 */

    /* 2. Build the production ROM_BASE candidate + snapshot (R11-C/D seams:
     * tilesets + layouts are published from this snapshot's streams). */
    Gen3ResourceDiagnostics_Init(&gdiag);
    memset(&info, 0, sizeof(info));
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &gdiag);
    CHECK(sessionError == EMERALD_SESSION_OK && candidate != NULL);
    if (sessionError != EMERALD_SESSION_OK || candidate == NULL)
    {
        printf("FAIL: BuildRomBaseCandidate (%d)\n", (int)sessionError);
        for (i = 0; i < gdiag.count; i++)
            printf("  diag %zu: reason=%s resource='%s' provider='%s'\n",
                   i, Gen3ResourceReason_Describe(gdiag.items[i].reason),
                   gdiag.items[i].resourceName, gdiag.items[i].providerId);
        Gen3ResourceDiagnostics_Destroy(&gdiag);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }
    CHECK(strcmp(info.providerId, EMERALD_ROM_BASE_PROVIDER_ID) == 0);
    CHECK(info.precedence == EMERALD_ROM_BASE_PRECEDENCE);
    CHECK(strcmp(info.providerVersion, "v1") == 0);
    CHECK(info.entryCount == 15750u); /* + 569 audio leaves + 202 structural + 530 song graphs (R12-B/C/D) + 1057 leaf (R13-B) + 5187 text (R13-C) + 3310 D1 */

    CHECK(Gen3ResourceCandidate_Build(candidate, &snapshot, &gdiag) && snapshot != NULL);
    Gen3ResourceDiagnostics_Destroy(&gdiag);
    if (snapshot == NULL)
    {
        printf("FAIL: candidate build\n");
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }

    /* 3. Publish the compat seams (trains R11-C tilesets + R11-D layouts;
     * object-event/Pokémon/trainer families publish alongside). */
    status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &cdiag);
    CHECK(status == EMERALD_COMPAT_OK);
    CHECK(cdiag.stage[0] == '\0');

    /* 4. The neighborhood module: Init AFTER publication (LayoutPublished). */
    NativeWorldNeighborhood_Init();

    TestRenderProof();

    /* Teardown mirroring the production harness's success path: the compat
     * seams' published pointers die with the snapshot, so everything is
     * destroyed only after the last test. */
    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);

    printf("harness D: %u checks, %u failures (%u tile entries)\n",
           sChecks, sFailures,
           (unsigned)(RENDER_NUM_BG * RENDER_BLOCKS_H * RENDER_BLOCKS_W * 4));
    return sFailures == 0u ? 0 : 1;
}
