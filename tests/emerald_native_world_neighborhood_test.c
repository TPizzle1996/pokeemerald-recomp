/* R11-E/F Harness A: NativeWorldNeighborhood — hand-built fixtures.
 *
 * Compiles the neighborhood module + host_memory.c + a stub
 * Overworld_GetMapHeaderByGroupAndId (the verbatim PORTABLE-branch body,
 * overworld.c:595-603) over a fixture gMapGroups GbaAddr table, with
 * hand-built MapHeader/MapLayout/MapEvents/connection records and a real
 * `struct SaveBlock1` fixture. Covers the pure math with fully controlled
 * inputs (plan §11 Harness A):
 *
 *   2  N/S math (cycle: Littleroot<->Route101 shape)
 *   3  E/W math (cycle with offset)
 *   4  positive offsets (south +10, west +6)
 *   5  negative offsets (north -5, south -100 Route122->Route123 shape)
 *   6  asymmetric dims (Route111 40x140 <-> Route113 100x20 shape)
 *   7  multiple neighbors (4 spatial, one in a second map group)
 *   8  duplicates + cycles (2x WEST like Route111; back-connection)
 *   12 outside-union -> BORDER parity incl. IMPASSABLE bit
 *   13 current precedence (boundary adjacency + adversarially corrupted
 *       overlapping record)
 *   21 5th-neighbor fail-closed: DEGRADED, records cleared, every query FALSE
 *   + null-gSaveBlock1Ptr gate + version-bump-on-READY-rebuild
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "fieldmap.h"
#include "overworld.h"
#include "constants/layouts.h"
#include "constants/map_groups.h"
#include "platform/host_memory.h"
#include "platform/native_world_neighborhood.h"

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

static int sFailures;
static int sChecks;

#define CHECK(cond)                                                     \
    do {                                                                \
        sChecks++;                                                      \
        if (!(cond)) {                                                  \
            sFailures++;                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b) CHECK((a) == (b))

/* ---------------------------------------------------------------- fixtures */

#define FIXTURE_MAX_METATILES 128

struct FixtureMap
{
    struct MapLayout layout;
    struct MapEvents events;
    struct MapHeader header;
    struct MapConnections conns;
    struct MapConnection connections[8];
    u16 *blocks; /* malloc'd width*height */
    u16 border[4];
    struct Tileset tileset;
    u16 metatiles[FIXTURE_MAX_METATILES * 8];
    u16 attrs[FIXTURE_MAX_METATILES];
};

#define FIXTURE_POOL 64
static struct FixtureMap sPool[FIXTURE_POOL];
static int sPoolCount;

/* group tables: gMapGroups[0] and gMapGroups[1] are exercised. */
#define FIXTURE_GROUP_TABLES 2
#define FIXTURE_GROUP_SLOTS 128
static GbaAddr sGroupTables[FIXTURE_GROUP_TABLES][FIXTURE_GROUP_SLOTS];
GbaAddr gMapGroups[MAP_GROUPS_COUNT]; /* PORTABLE-branch fixture table */

static struct SaveBlock1 gSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &gSaveBlock1;

static struct FixtureMap *MakeFixture(u16 width, u16 height, int idBase)
{
    struct FixtureMap *f;
    int x, y;

    if (sPoolCount >= FIXTURE_POOL)
    {
        fprintf(stderr, "fixture pool exhausted\n");
        sFailures++;
        return NULL;
    }
    f = &sPool[sPoolCount++];

    f->blocks = malloc((size_t)width * height * sizeof(u16));
    if (f->blocks == NULL)
    {
        fprintf(stderr, "fixture malloc failed\n");
        sFailures++;
        return NULL;
    }
    f->layout.width = width;
    f->layout.height = height;
    f->layout.border = f->border;
    f->layout.map = f->blocks;
    f->layout.primaryTileset = &f->tileset;
    f->layout.secondaryTileset = NULL;

    f->tileset.isCompressed = FALSE;
    f->tileset.isSecondary = FALSE;
    f->tileset.tiles = NULL;
    f->tileset.palettes = NULL;
    f->tileset.metatiles = f->metatiles;
    f->tileset.metatileAttributes = f->attrs;
    f->tileset.callback = NULL;

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            u16 id = idBase + (x + y * width) % 8;
            f->blocks[y * width + x] = PACK_METATILE(id)
                                     | PACK_COLLISION(2)
                                     | PACK_ELEVATION(3);
        }
    }
    for (x = 0; x < 4; x++)
        f->border[x] = 0x1000 + x;

    /* metatile tables: id -> (layerType = (id/8)%3, behavior = id), tiles =
     * id*8 + q + 1 so every tile entry is distinguishable. */
    for (x = 0; x < FIXTURE_MAX_METATILES; x++)
    {
        int q;
        f->attrs[x] = PACK_LAYER_TYPE((x / 8) % 3) | PACK_BEHAVIOR(x);
        for (q = 0; q < 8; q++)
            f->metatiles[x * 8 + q] = x * 8 + q + 1;
    }

    f->header.mapLayout = &f->layout;
    f->header.events = &f->events;
    f->header.mapScripts = NULL;
    f->header.connections = &f->conns;
    f->header.mapLayoutId = 0; /* not synthetic */
    return f;
}

static void SetConnections(struct FixtureMap *f, int count,
                           u8 dir0, s32 off0, u8 g0, u8 n0,
                           u8 dir1, s32 off1, u8 g1, u8 n1,
                           u8 dir2, s32 off2, u8 g2, u8 n2,
                           u8 dir3, s32 off3, u8 g3, u8 n3,
                           u8 dir4, s32 off4, u8 g4, u8 n4)
{
    const struct { u8 dir; s32 off; u8 g; u8 n; } args[8] = {
        { dir0, off0, g0, n0 }, { dir1, off1, g1, n1 },
        { dir2, off2, g2, n2 }, { dir3, off3, g3, n3 },
        { dir4, off4, g4, n4 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
    };
    int i;
    f->conns.count = count;
    f->conns.connections = f->connections;
    for (i = 0; i < count; i++)
    {
        f->connections[i].direction = args[i].dir;
        f->connections[i].offset = args[i].off;
        f->connections[i].mapGroup = args[i].g;
        f->connections[i].mapNum = args[i].n;
    }
}

/* Register fixture f at (group, num) and wire the GbaAddr table. */
static void RegisterFixture(struct FixtureMap *f, u8 group, u8 num)
{
    if (group >= FIXTURE_GROUP_TABLES || num >= FIXTURE_GROUP_SLOTS)
    {
        fprintf(stderr, "fixture group slot out of range\n");
        sFailures++;
        return;
    }
    sGroupTables[group][num] = HostPointerToGbaAddr(&f->header);
    gMapGroups[group] = HostPointerToGbaAddr(sGroupTables[group]);
}

/* Verbatim PORTABLE-branch body, overworld.c:595-603. */
struct MapHeader const *const Overworld_GetMapHeaderByGroupAndId(u16 mapGroup, u16 mapNum)
{
    const GbaAddr *mapGroupTable = HostResolveGbaAddr(gMapGroups[mapGroup]);
    return HostResolveGbaTableEntry(mapGroupTable, mapNum);
}

static void SetLocation(u8 group, u8 num)
{
    gSaveBlock1.location.mapGroup = group;
    gSaveBlock1.location.mapNum = num;
}

static const struct NativeWorldNeighborhood *Build(void)
{
    const struct NativeWorldNeighborhood *nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    return nb;
}

/* Build expecting a specific (non-READY) terminal state: the fail-closed
   tests (5th neighbor, synthetic map) assert DEGRADED explicitly. */
static const struct NativeWorldNeighborhood *BuildExpect(enum NativeWorldNeighborhoodStatus status)
{
    const struct NativeWorldNeighborhood *nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, status);
    return nb;
}

/* ------------------------------------------------------------------ tests */

static void TestNullGate(void)
{
    struct SaveBlock1 *saved = gSaveBlock1Ptr;
    const struct NativeWorldNeighborhood *nb;

    NativeWorldNeighborhood_Init();
    gSaveBlock1Ptr = NULL;
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_UNINITIALIZED);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, 0, NULL, NULL, NULL,
                                                  NULL, NULL, NULL), FALSE);
    gSaveBlock1Ptr = saved;
    NativeWorldNeighborhood_Shutdown();
}

static void TestNSMath(void)
{
    struct FixtureMap *a = MakeFixture(40, 20, 1);  /* Littleroot shape */
    struct FixtureMap *b = MakeFixture(40, 20, 33); /* Route101 shape */
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;
    u16 block;
    u16 metatileId;
    u8 layerType;
    const struct MapLayout *layout;
    u16 expectBlock;

    SetConnections(a, 1, CONNECTION_NORTH, 0, 0, 1, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 0);
    /* b has the back-connection: SOUTH -> a at offset 0 (cycle). */
    SetConnections(b, 1, CONNECTION_SOUTH, 0, 0, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(b, 0, 1);

    /* a current: neighbor b at (0,-20). */
    SetLocation(0, 0);
    nb = Build();
    CHECK_EQ_INT(nb->neighborCount, 1);
    CHECK_EQ_INT(nb->neighbors[0].worldX, 0);
    CHECK_EQ_INT(nb->neighbors[0].worldY, -20);
    CHECK_EQ_INT(nb->neighbors[0].mapGroup, 0);
    CHECK_EQ_INT(nb->neighbors[0].mapNum, 1);
    CHECK_EQ_INT(nb->neighbors[0].width, 40);
    CHECK_EQ_INT(nb->neighbors[0].height, 20);
    CHECK_EQ_INT(nb->worldMinX, 0);
    CHECK_EQ_INT(nb->worldMinY, -20);
    CHECK_EQ_INT(nb->worldMaxX, 40);
    CHECK_EQ_INT(nb->worldMaxY, 20);
    CHECK_EQ_INT(nb->version, 1);

    /* world (5,-1) -> b local (5,19); block identity from b's pattern. */
    expectBlock = b->blocks[19 * 40 + 5];
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, -1, &src, &g, &n,
                                                  &lx, &ly, &block), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(g, 0);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(lx, 5);
    CHECK_EQ_INT(ly, 19);
    CHECK_EQ_INT(block, expectBlock);

    /* ResolveBlockForRender on the neighbor cell: owner layout = b's,
     * metatile id unpacked from b's block word, layerType from b's attrs. */
    CHECK_EQ_INT(NativeWorldNeighborhood_ResolveBlockForRender(nb, 5, -1, NULL,
                                                               &g, &n, NULL, NULL,
                                                               &layout, &metatileId,
                                                               &layerType), TRUE);
    CHECK(layout == &b->layout);
    CHECK_EQ_INT(g, 0);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(metatileId, UNPACK_METATILE(expectBlock));
    CHECK_EQ_INT(layerType, UNPACK_LAYER_TYPE(b->attrs[metatileId]));

    /* top row: (5,-20) -> b local (5,0). */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, -20, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 5);
    CHECK_EQ_INT(ly, 0);

    /* b current (cycle): a at (0, +20) via the back-connection. */
    SetLocation(0, 1);
    nb = Build();
    CHECK_EQ_INT(nb->neighborCount, 1);
    CHECK_EQ_INT(nb->neighbors[0].worldX, 0);
    CHECK_EQ_INT(nb->neighbors[0].worldY, 20);
    CHECK_EQ_INT(nb->neighbors[0].mapNum, 0);
    CHECK_EQ_INT(nb->version, 2); /* READY rebuild bumps */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, 20, &src, &g, &n,
                                                  &lx, &ly, &block), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(g, 0);
    CHECK_EQ_INT(n, 0);
    CHECK_EQ_INT(lx, 5);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(block, a->blocks[0 * 40 + 5]);

    /* bottom row of b: (5,39) -> a local (5,19). */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, 39, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(ly, 19);
}

static void TestEWMath(void)
{
    struct FixtureMap *e = MakeFixture(30, 15, 1);
    struct FixtureMap *f = MakeFixture(20, 15, 33);
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    s32 lx, ly;

    SetConnections(e, 1, CONNECTION_EAST, 3, 0, 1, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(e, 0, 2);
    SetConnections(f, 1, CONNECTION_WEST, -3, 0, 2, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(f, 0, 3);

    /* e current: f at (30, 3). */
    SetLocation(0, 2);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, 30);
    CHECK_EQ_INT(nb->neighbors[0].worldY, 3);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 30, 4, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 1);

    /* f current (cycle): e at (-30, -3) (e is 30 blocks wide). */
    SetLocation(0, 3);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, -30);
    CHECK_EQ_INT(nb->neighbors[0].worldY, -3);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -30, -2, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 1);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -1, 3, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 29);
    CHECK_EQ_INT(ly, 6);  /* e's rect starts at world y=-3: local = 3 - (-3) */
}

static void TestPositiveOffsets(void)
{
    struct FixtureMap *g = MakeFixture(25, 10, 1);
    struct FixtureMap *h = MakeFixture(25, 15, 33);
    struct FixtureMap *i = MakeFixture(20, 12, 65);
    struct FixtureMap *j = MakeFixture(15, 12, 97);
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    s32 lx, ly;

    SetConnections(g, 1, CONNECTION_SOUTH, 10, 0, 5, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(g, 0, 4);
    SetConnections(h, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(h, 0, 5);

    SetLocation(0, 4);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, 10);   /* SOUTH (o, +H) */
    CHECK_EQ_INT(nb->neighbors[0].worldY, 10);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 10, 10, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 34, 24, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 24);
    CHECK_EQ_INT(ly, 14);

    /* WEST with a positive vertical offset: j at (-15, 6). */
    SetConnections(i, 1, CONNECTION_WEST, 6, 0, 7, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(i, 0, 6);
    SetConnections(j, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(j, 0, 7);

    SetLocation(0, 6);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, -15);  /* WEST (-cW, o) */
    CHECK_EQ_INT(nb->neighbors[0].worldY, 6);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -15, 6, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
}

static void TestNegativeOffsets(void)
{
    struct FixtureMap *k = MakeFixture(30, 18, 1);
    struct FixtureMap *l = MakeFixture(30, 12, 33);
    struct FixtureMap *m = MakeFixture(20, 20, 65);
    struct FixtureMap *n = MakeFixture(20, 10, 97);
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    s32 lx, ly;

    SetConnections(k, 1, CONNECTION_NORTH, -5, 0, 9, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(k, 0, 8);
    SetConnections(l, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(l, 0, 9);

    /* NORTH at -5: l at (-5, -12). */
    SetLocation(0, 8);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, -5);
    CHECK_EQ_INT(nb->neighbors[0].worldY, -12);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -5, -12, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -1, -1, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 4);
    CHECK_EQ_INT(ly, 11);

    /* SOUTH at -100 (Route122->Route123 shape): n far left of the map. */
    SetConnections(m, 1, CONNECTION_SOUTH, -100, 0, 11, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(m, 0, 10);
    SetConnections(n, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(n, 0, 11);

    SetLocation(0, 10);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, -100);
    CHECK_EQ_INT(nb->neighbors[0].worldY, 20);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -100, 20, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -81, 29, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 19);
    CHECK_EQ_INT(ly, 9);
    /* (5,30): inside m's x span, but n's rect ends at y=30 -> BORDER. */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -81, 30, &src, NULL, NULL,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, 30, &src, NULL, NULL,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);
}

static void TestAsymmetricDims(void)
{
    struct FixtureMap *a = MakeFixture(40, 140, 1);  /* Route111 shape */
    struct FixtureMap *b = MakeFixture(100, 20, 33); /* Route113 shape */
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    s32 lx, ly;

    SetConnections(a, 1, CONNECTION_WEST, 0, 0, 13, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 12);
    SetConnections(b, 1, CONNECTION_EAST, 0, 0, 12, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(b, 0, 13);

    SetLocation(0, 12);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, -100);   /* WEST (-cW, o) */
    CHECK_EQ_INT(nb->neighbors[0].worldY, 0);
    CHECK_EQ_INT(nb->worldMinX, -100);
    CHECK_EQ_INT(nb->worldMinY, 0);
    CHECK_EQ_INT(nb->worldMaxX, 40);
    CHECK_EQ_INT(nb->worldMaxY, 140);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -100, 0, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -1, 19, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 99);
    CHECK_EQ_INT(ly, 19);
    /* (-1,20): b's rect y<20, a's rect x>=0 -> BORDER. */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -1, 20, &src, NULL, NULL,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);

    /* GetBounds matches the union. */
    {
        s32 minX, minY, maxX, maxY;
        CHECK_EQ_INT(NativeWorldNeighborhood_GetBounds(nb, &minX, &minY, &maxX, &maxY), TRUE);
        CHECK_EQ_INT(minX, -100);
        CHECK_EQ_INT(minY, 0);
        CHECK_EQ_INT(maxX, 40);
        CHECK_EQ_INT(maxY, 140);
    }
}

static void TestMultipleNeighbors(void)
{
    struct FixtureMap *a = MakeFixture(30, 10, 1);
    struct FixtureMap *b = MakeFixture(30, 12, 33);
    struct FixtureMap *c = MakeFixture(20, 10, 65);
    struct FixtureMap *d = MakeFixture(40, 10, 97);
    struct FixtureMap *e = MakeFixture(30, 8, 1 + 128);
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    s32 lx, ly;

    SetConnections(a, 4, CONNECTION_SOUTH, 0, 0, 20, CONNECTION_WEST, 0, 1, 21,
                   CONNECTION_EAST, 0, 1, 22, CONNECTION_NORTH, 0, 0, 23,
                   0,0,0,0);
    RegisterFixture(a, 0, 30);
    RegisterFixture(b, 0, 20);
    RegisterFixture(c, 1, 21); /* second map group: accessor plumbing */
    RegisterFixture(d, 1, 22);
    RegisterFixture(e, 0, 23);

    SetLocation(0, 30);
    nb = Build();
    CHECK_EQ_INT(nb->neighborCount, 4);
    CHECK_EQ_INT(nb->neighbors[0].worldX, 0);    /* SOUTH */
    CHECK_EQ_INT(nb->neighbors[0].worldY, 10);
    CHECK_EQ_INT(nb->neighbors[1].worldX, -20);  /* WEST (group 1) */
    CHECK_EQ_INT(nb->neighbors[1].worldY, 0);
    CHECK_EQ_INT(nb->neighbors[1].mapGroup, 1);
    CHECK_EQ_INT(nb->neighbors[2].worldX, 30);   /* EAST (group 1) */
    CHECK_EQ_INT(nb->neighbors[2].worldY, 0);
    CHECK_EQ_INT(nb->neighbors[2].mapGroup, 1);
    CHECK_EQ_INT(nb->neighbors[3].worldX, 0);    /* NORTH */
    CHECK_EQ_INT(nb->neighbors[3].worldY, -8);
    CHECK_EQ_INT(nb->worldMinX, -20);
    CHECK_EQ_INT(nb->worldMinY, -8);
    CHECK_EQ_INT(nb->worldMaxX, 70);
    CHECK_EQ_INT(nb->worldMaxY, 22);

    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, 10, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -20, 0, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 30, 0, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, -8, &src, NULL, NULL,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
}

static void TestDuplicatesAndCycles(void)
{
    struct FixtureMap *a = MakeFixture(40, 20, 1);  /* Route111 shape: 2x WEST */
    struct FixtureMap *b = MakeFixture(20, 20, 33); /* Route113 shape (o 0) */
    struct FixtureMap *c = MakeFixture(40, 20, 65); /* Route112 shape (o 20) */
    struct FixtureMap *x = MakeFixture(20, 20, 97); /* B8's SOUTH target */
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    s32 lx, ly;
    u8 g, n;

    SetConnections(a, 2, CONNECTION_WEST, 0, 0, 41, CONNECTION_WEST, 20, 0, 42,
                   0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 40);
    SetConnections(b, 2, CONNECTION_EAST, 0, 0, 40, CONNECTION_SOUTH, 0, 0, 43,
                   0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(b, 0, 41);
    SetConnections(c, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(c, 0, 42);
    SetConnections(x, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(x, 0, 43);

    /* a current: two WEST records, distinct non-overlapping origins. */
    SetLocation(0, 40);
    nb = Build();
    CHECK_EQ_INT(nb->neighborCount, 2);
    CHECK_EQ_INT(nb->neighbors[0].worldX, -20);   /* b: (-cW, o) */
    CHECK_EQ_INT(nb->neighbors[0].worldY, 0);
    CHECK_EQ_INT(nb->neighbors[1].worldX, -40);   /* c: (-cW, o) */
    CHECK_EQ_INT(nb->neighbors[1].worldY, 20);

    /* declaration-order precedence on the shared x span: y<20 -> b, y>=20 -> c. */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -10, 10, &src, &g, &n,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(n, 41);
    CHECK_EQ_INT(lx, 10);
    CHECK_EQ_INT(ly, 10);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -30, 30, &src, &g, &n,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(n, 42);
    CHECK_EQ_INT(lx, 10);
    CHECK_EQ_INT(ly, 10);
    /* (-30,10): inside c's x span but c's rect y>=20, b's rect x>=-20 -> BORDER. */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, -30, 10, &src, NULL, NULL,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);

    /* cycle: b current -> a at (20,0) via EAST back-connection, x at (0,20). */
    SetLocation(0, 41);
    nb = Build();
    CHECK_EQ_INT(nb->neighborCount, 2);
    CHECK_EQ_INT(nb->neighbors[0].worldX, 20);    /* EAST -> a */
    CHECK_EQ_INT(nb->neighbors[0].worldY, 0);
    CHECK_EQ_INT(nb->neighbors[1].worldX, 0);     /* SOUTH -> x */
    CHECK_EQ_INT(nb->neighbors[1].worldY, 20);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 20, 0, &src, &g, &n,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(g, 0);
    CHECK_EQ_INT(n, 40);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, 20, &src, &g, &n,
                                                  &lx, &ly, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(n, 43);
}

static void TestBorderParity(void)
{
    struct FixtureMap *a = MakeFixture(40, 20, 1);
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    u16 block;
    u8 g, n;
    s32 lx, ly;

    /* a's border: [0x1000, 0x1001, 0x1002, 0x1003]. */
    SetConnections(a, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 50);

    SetLocation(0, 50);
    nb = Build();
    CHECK_EQ_INT(nb->neighborCount, 0);
    CHECK_EQ_INT(nb->worldMinX, 0);
    CHECK_EQ_INT(nb->worldMinY, 0);
    CHECK_EQ_INT(nb->worldMaxX, 40);
    CHECK_EQ_INT(nb->worldMaxY, 20);

#define BORDER_EXPECT(x, y, word)                                           \
    do {                                                                    \
        s32 xBk = (x) + MAP_OFFSET;                                         \
        s32 yBk = (y) + MAP_OFFSET;                                         \
        s32 bi = ((xBk + 1) & 1) + ((yBk + 1) & 1) * 2;                     \
        CHECK_EQ_INT((word) & 0x0FFF, bi); /* word == 0x1000+bi, the fixture border */ \
        CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, (x), (y), &src,   \
                                                      &g, &n, &lx, &ly,     \
                                                      &block), TRUE);       \
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);                       \
        CHECK_EQ_INT(g, 0);                                                 \
        CHECK_EQ_INT(n, 50);                                                \
        CHECK_EQ_INT(lx, (x));                                              \
        CHECK_EQ_INT(ly, (y));                                              \
        CHECK_EQ_INT(block, ((word) | MAPGRID_IMPASSABLE));                 \
    } while (0)

    /* (0,-1): xBk 7, yBk 6 -> i = 0 + 2 = 2 -> border[2]. */
    BORDER_EXPECT(0, -1, 0x1002);
    /* (1,20): xBk 8, yBk 27 -> i = 1 + 0 = 1 -> border[1]. */
    BORDER_EXPECT(1, 20, 0x1001);
    /* (40,5): xBk 47, yBk 12 -> i = 0 + 2 = 2 -> border[2]. */
    BORDER_EXPECT(40, 5, 0x1002);
    /* (-1,0): xBk 6, yBk 7 -> i = 1 + 0 = 1 -> border[1]. */
    BORDER_EXPECT(-1, 0, 0x1001);
    /* (-1,-1): xBk 6, yBk 6 -> i = 1 + 2 = 3 -> border[3] (signed parity,
       documented forward extension for the camera-invisible west/north). */
    BORDER_EXPECT(-1, -1, 0x1003);
    /* (0,-2): xBk 7, yBk 5 -> i = 0 + 0 = 0 -> border[0]. */
    BORDER_EXPECT(0, -2, 0x1000);
    /* (2,21): xBk 9, yBk 28 -> i = 0 + 2 = 2 -> border[2]. */
    BORDER_EXPECT(2, 21, 0x1002);

    /* GetBounds for a current-only map: (0,0,w,h). */
    {
        s32 minX, minY, maxX, maxY;
        CHECK_EQ_INT(NativeWorldNeighborhood_GetBounds(nb, &minX, &minY, &maxX, &maxY), TRUE);
        CHECK_EQ_INT(minX, 0);
        CHECK_EQ_INT(minY, 0);
        CHECK_EQ_INT(maxX, 40);
        CHECK_EQ_INT(maxY, 20);
    }

    /* ResolveBlockForRender on a BORDER cell: owner = current layout, the
       border word's metatile id, layerType from the current tileset. */
    {
        const struct MapLayout *layout;
        u16 metatileId;
        u8 layerType;
        CHECK_EQ_INT(NativeWorldNeighborhood_ResolveBlockForRender(nb, 0, -1, &src,
                                                                   NULL, NULL, NULL, NULL,
                                                                   &layout, &metatileId,
                                                                   &layerType), TRUE);
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);
        CHECK(layout == &a->layout);
        CHECK_EQ_INT(metatileId, UNPACK_METATILE(0x1002));
        CHECK_EQ_INT(layerType, UNPACK_LAYER_TYPE(a->attrs[UNPACK_METATILE(0x1002)]));
    }
}

static void TestCurrentPrecedence(void)
{
    struct FixtureMap *a = MakeFixture(40, 20, 1);
    struct FixtureMap *b = MakeFixture(40, 20, 33);
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    u8 g, n;

    SetConnections(a, 1, CONNECTION_NORTH, 0, 0, 61, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 60);
    SetConnections(b, 1, CONNECTION_SOUTH, 0, 0, 60, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(b, 0, 61);

    SetLocation(0, 60);
    nb = Build();

    /* Boundary adjacency: the neighbor rect is [0,40)x[-20,0) and the current
       rect [0,40)x[0,20). The half-open conventions leave the boundary rows
       to exactly one owner: row -1 is the neighbor, row 0 is CURRENT even
       though the neighbor is declared first in the connections list. */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, -1, &src, &g, &n,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(n, 61);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, 0, &src, &g, &n,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
    CHECK_EQ_INT(n, 60);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 39, 19, &src, &g, &n,
                                                  NULL, NULL, NULL), TRUE);
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);

    /* Adversarial overlap: corrupt the built state so the neighbor record's
       rect covers the current map's interior (a fixture geometry the formulas
       can never produce - NORTH/SOUTH are y-disjoint and WEST/EAST x-disjoint
       for any positive dims). The query must STILL serve CURRENT for every
       cell inside the current rect: the current-rect check runs first, so an
       overlapping record can never steal current cells (precedence by check
       order, matching gameplay's selection semantics). */
    {
        struct NativeWorldNeighborhood *mutable = (struct NativeWorldNeighborhood *)nb;
        mutable->neighbors[0].worldX = 0;
        mutable->neighbors[0].worldY = -10;
        mutable->neighbors[0].width = 40;
        mutable->neighbors[0].height = 30; /* covers y[-10,20): overlaps current */

        CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, 5, &src, &g, &n,
                                                      NULL, NULL, NULL), TRUE);
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
        CHECK_EQ_INT(g, 0);
        CHECK_EQ_INT(n, 60);
        CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, 0, &src, NULL, NULL,
                                                      NULL, NULL, NULL), TRUE);
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
        /* y=-5 is outside the current rect but inside the corrupted record ->
           the record serves it (neighbor path). */
        CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, -5, &src, &g, &n,
                                                      NULL, NULL, NULL), TRUE);
        CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
        CHECK_EQ_INT(n, 61);
        /* The GetObjectEventsAt accessor follows the same precedence. */
        CHECK_EQ_INT(NativeWorldNeighborhood_GetObjectEventsAt(nb, 5, 5, &g) ==
                     a->header.events, TRUE);
        CHECK_EQ_INT(NativeWorldNeighborhood_GetObjectEventsAt(nb, 5, -5, &g) ==
                     b->header.events, TRUE);
    }

    /* Hot path: an unchanged identity does NOT rebuild, so the corrupted
       record persists across GetState calls (lazy diff semantics). */
    {
        const struct NativeWorldNeighborhood *nb2 = NativeWorldNeighborhood_GetState();
        CHECK(nb2 == nb);
        CHECK_EQ_INT(nb2->neighbors[0].worldY, -10);
    }

    /* An identity change rebuilds cleanly, restoring the honest record. */
    SetLocation(0, 61);
    nb = Build();
    CHECK_EQ_INT(nb->neighbors[0].worldX, 0);
    CHECK_EQ_INT(nb->neighbors[0].worldY, 20);
    CHECK_EQ_INT(nb->neighbors[0].width, 40);
    CHECK_EQ_INT(nb->neighbors[0].height, 20);
}

static void TestFifthNeighborFailsClosed(void)
{
    struct FixtureMap *a = MakeFixture(40, 20, 1);
    const struct NativeWorldNeighborhood *nb;
    u32 versionBefore;
    u8 g;

    /* Build a READY neighborhood first so version is observable. The version
       is cumulative across the whole process (every earlier READY rebuild
       bumped it), so capture it dynamically, never assert an absolute value. */
    SetConnections(a, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 70);
    SetLocation(0, 70);
    nb = Build();
    versionBefore = nb->version;

    /* Five spatial connections: the builder must DEGRADE, never truncate. */
    SetConnections(a, 5, CONNECTION_NORTH, 0, 0, 71, CONNECTION_SOUTH, 0, 0, 72,
                   CONNECTION_WEST, 0, 0, 73, CONNECTION_EAST, 0, 0, 74,
                   CONNECTION_NORTH, 8, 0, 75);
    for (g = 71; g <= 75; g++)
    {
        struct FixtureMap *leaf = MakeFixture(20, 20, 33 + g * 8);
        SetConnections(leaf, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
        RegisterFixture(leaf, 0, g);
    }

    NativeWorldNeighborhood_Invalidate(); /* identity unchanged -> hot path would skip the rebuild */
    nb = BuildExpect(NATIVE_WORLD_NB_DEGRADED); /* now 5 spatial neighbors -> DEGRADED */
    CHECK_EQ_INT(nb->neighborCount, 4); /* the 4 records built before the 5th stay, but
                                           every query is gated on READY - fail-closed */
    CHECK_EQ_INT(nb->currentBlocksSynthetic, FALSE);
    CHECK_EQ_INT(nb->version, versionBefore);      /* no version bump on failure */
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 0, 0, NULL, NULL, NULL,
                                                  NULL, NULL, NULL), FALSE);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, -1, NULL, NULL, NULL,
                                                  NULL, NULL, NULL), FALSE);
    CHECK_EQ_INT(NativeWorldNeighborhood_ResolveBlockForRender(nb, 0, 0, NULL,
                                                               NULL, NULL, NULL, NULL,
                                                               NULL, NULL, NULL), FALSE);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBounds(nb, NULL, NULL, NULL, NULL), FALSE);
    CHECK(NativeWorldNeighborhood_GetObjectEventsAt(nb, 0, 0, &g) == NULL);
    CHECK_EQ_INT(g, 0);

    /* Recovery: remove the 5th connection -> next access rebuilds READY. */
    SetConnections(a, 4, CONNECTION_NORTH, 0, 0, 71, CONNECTION_SOUTH, 0, 0, 72,
                   CONNECTION_WEST, 0, 0, 73, CONNECTION_EAST, 0, 0, 74,
                   0,0,0,0);
    NativeWorldNeighborhood_Invalidate(); /* identity unchanged -> hot path would skip the rebuild */
    nb = Build();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 4);
    CHECK_EQ_INT(nb->version, versionBefore + 1);
}

static void TestObjectEventsViews(void)
{
    struct FixtureMap *a = MakeFixture(30, 10, 1);
    struct FixtureMap *b = MakeFixture(30, 10, 33);
    const struct NativeWorldNeighborhood *nb;
    u8 count;
    const struct MapEvents *events;

    a->events.objectEventCount = 3;
    b->events.objectEventCount = 7;

    SetConnections(a, 1, CONNECTION_SOUTH, 0, 0, 81, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(a, 0, 80);
    SetConnections(b, 0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    RegisterFixture(b, 0, 81);

    SetLocation(0, 80);
    nb = Build();

    events = NativeWorldNeighborhood_GetObjectEventsAt(nb, 5, 5, &count);
    CHECK(events == &a->events);
    CHECK_EQ_INT(count, 3);

    events = NativeWorldNeighborhood_GetObjectEventsAt(nb, 5, 10, &count);
    CHECK(events == &b->events);
    CHECK_EQ_INT(count, 7);

    events = NativeWorldNeighborhood_GetObjectEventsAt(nb, 5, -1, &count);
    CHECK(events == NULL);
    CHECK_EQ_INT(count, 0);

    /* synthetic gate: pyramid layout id -> DEGRADED, events NULL. */
    a->header.mapLayoutId = LAYOUT_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR;
    NativeWorldNeighborhood_Invalidate(); /* identity unchanged -> hot path would skip the rebuild */
    nb = BuildExpect(NATIVE_WORLD_NB_DEGRADED);
    CHECK_EQ_INT(nb->currentBlocksSynthetic, TRUE);
    events = NativeWorldNeighborhood_GetObjectEventsAt(nb, 5, 5, &count);
    CHECK(events == NULL);
    CHECK_EQ_INT(count, 0);
    CHECK_EQ_INT(NativeWorldNeighborhood_GetBlock(nb, 5, 5, NULL, NULL, NULL,
                                                  NULL, NULL, NULL), FALSE);
    a->header.mapLayoutId = 0;
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    NativeWorldNeighborhood_Init();

    TestNullGate();
    TestNSMath();
    TestEWMath();
    TestPositiveOffsets();
    TestNegativeOffsets();
    TestAsymmetricDims();
    TestMultipleNeighbors();
    TestDuplicatesAndCycles();
    TestBorderParity();
    TestCurrentPrecedence();
    TestFifthNeighborFailsClosed();
    TestObjectEventsViews();

    NativeWorldNeighborhood_Shutdown();

    printf("harness A: %d checks, %d failures\n", sChecks, sFailures);
    if (sFailures != 0)
        return 1;
    return 0;
}
