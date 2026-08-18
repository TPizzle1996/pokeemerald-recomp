/* R11-E/F: Harness B — the neighborhood module against the REAL qualified
 * Emerald data + the REAL production pack (plan §11 Harness B, lines 528-550).
 *
 * Link set (run_emerald_native_world_real.sh): the production-runner link set
 * (gen3 core + all compat seams + umbrella + session, as
 * run_emerald_trainer_native_compat_production.sh) PLUS the real compiled map
 * tables (build/linux64/data/maps.o + map_events.o + event_scripts.o — the
 * mutually self-contained closure verified for Harness B, see the plan's §10
 * link analysis), host_memory.c, the tileset-anim stubs
 * (emerald_tileset_compat_stubs.c), the Overworld accessor stub
 * (emerald_native_world_overworld_stub.c) and this file.
 *
 * The session drive publishes the R11-C tileset + R11-D layout seams from the
 * production pack (games/emerald/base/emerald-bpee01-v1.rpack, 5087 entries:
 * 196 trainer + 1608 Pokémon battle + 288 object-event + 1544 tileset + 882
 * layout + 569 R12-B audio leaves; the session covers all 6 family catalogs
 * = 5087) BEFORE NativeWorldNeighborhood_Init: the builder's
 * LayoutPublished gate requires every map's layout record to carry published
 * .map/.border and a published primary tileset (real blockdata + metatiles
 * from the pack).
 *
 * Tests (plan §11 Harness B): 1 (Littleroot<->Route101 both directions:
 * origins (0,-20)/(0,+20), Oldale), 9 (no-connection interior:
 * LittlerootTown_MaysHouse_1F — neighborCount 0), 10 (query inside current),
 * 11 (query inside neighbor), 14 (identity mutation -> version bump +
 * rebuild), 17 (no second-hop: Route101 residents {Route101, Oldale,
 * Littleroot} only), 18 (Route124 -> 4 spatial neighbors, dive excluded),
 * 19 (data-wide pin sweep: 148 records = 134 spatial + 7 dive + 7 emerge;
 * 64 maps with >=1 connection, 61 with >=1 spatial, 3 dive/emerge-only;
 * max 4 spatial/map; max 2 same-direction; 0 back-connection violations;
 * 18 negative offsets; duplicate pairs' rects non-overlapping), 20
 * (synthetic pyramid -> DEGRADED + currentBlocksSynthetic TRUE, neighborCount
 * 0, version UNCHANGED, every query FALSE inside AND outside the rect).
 *
 * Map identities are data pins from data/maps/map_groups.json (the generator
 * source of groups.inc + maps.h): group order is the file's "group_order"
 * (== gMapGroups table order), mapNum is the index within the group's list.
 * Every identity is cross-checked against the compiled tables via the
 * accessor before use (layout dims below).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "fieldmap.h"
#include "overworld.h"
#include "constants/layouts.h"
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

/* map_groups.json identities used by tests 1/9/17/18/20 (see the header
 * comment; each is cross-checked against the compiled tables below). */
#define MAP_GROUP_TOWNS_AND_ROUTES 0
#define MAP_NUM_LITTLEROOT_TOWN 9
#define MAP_NUM_OLDALE_TOWN 10
#define MAP_NUM_ROUTE101 16
#define MAP_NUM_ROUTE102 17
#define MAP_NUM_ROUTE103 18
#define MAP_NUM_ROUTE111 26
#define MAP_NUM_ROUTE124 39
#define MAP_NUM_ROUTE125 40
#define MAP_NUM_ROUTE126 41
#define MAP_NUM_MAUVILLE_CITY 2
#define MAP_NUM_LILYCOVE_CITY 5
#define MAP_NUM_MOSSDEEP_CITY 6
#define MAP_NUM_UNDERWATER_ROUTE124 50
#define MAP_GROUP_INDOOR_LITTLEROOT 1
#define MAP_NUM_LITTLEROOT_TOWN_MAYS_HOUSE_1F 2
#define MAP_GROUP_SPECIAL_AREA 26
#define MAP_NUM_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR 26

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

/* The audit-verified world-origin formula (plan §1, Fill*Connection-verified):
 * NORTH (o, -cH) SOUTH (o, +curH) WEST (-cW, o) EAST (+curW, o), where
 * curW/curH are the ACTIVE map's layout dims and cW/cH the neighbor's. The
 * test applies it independently over the raw connection records; the module
 * must produce the same rects. */
static void ExpectedRect(u8 direction, s32 offset, s32 curW, s32 curH,
                         s32 cW, s32 cH, s32 *x, s32 *y)
{
    switch (direction)
    {
    case CONNECTION_NORTH:
        *x = offset;
        *y = -cH;
        break;
    case CONNECTION_SOUTH:
        *x = offset;
        *y = curH;
        break;
    case CONNECTION_WEST:
        *x = -cW;
        *y = offset;
        break;
    case CONNECTION_EAST:
        *x = curW;
        *y = offset;
        break;
    default:
        *x = 0;
        *y = 0;
        break;
    }
}

static bool32 IsSpatialDirection(u8 direction)
{
    return direction == CONNECTION_NORTH || direction == CONNECTION_SOUTH
        || direction == CONNECTION_WEST || direction == CONNECTION_EAST;
}

static bool32 OppositeDirections(u8 a, u8 b)
{
    return (a == CONNECTION_NORTH && b == CONNECTION_SOUTH)
        || (a == CONNECTION_SOUTH && b == CONNECTION_NORTH)
        || (a == CONNECTION_WEST && b == CONNECTION_EAST)
        || (a == CONNECTION_EAST && b == CONNECTION_WEST);
}

/* The module's synthetic-grid predicate (module internals): battle pyramid
 * floor + trainer-hill 1F-4F. Replicated here so the sweep can predict the
 * status from the compiled layout id alone. */
static bool32 IsSyntheticLayoutId(u16 mapLayoutId)
{
    if (mapLayoutId == LAYOUT_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR)
        return TRUE;
    if (mapLayoutId == LAYOUT_TRAINER_HILL_1F
     || mapLayoutId == LAYOUT_TRAINER_HILL_2F
     || mapLayoutId == LAYOUT_TRAINER_HILL_3F
     || mapLayoutId == LAYOUT_TRAINER_HILL_4F)
        return TRUE;
    return FALSE;
}

/* ---------------------------------------------------- test 1: Littleroot */
static void TestLittlerootRoute101(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct NativeWorldNeighborRecord *r;
    const struct MapLayout *litLayout;
    const struct MapLayout *r101Layout;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;
    u16 block;

    /* Identity pins vs the compiled tables (map_groups.json -> accessor). */
    litLayout = HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN)->mapLayout;
    CHECK_EQ_INT(litLayout->width, 20);
    CHECK_EQ_INT(litLayout->height, 20);
    r101Layout = HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE101)->mapLayout;
    CHECK_EQ_INT(r101Layout->width, 20);
    CHECK_EQ_INT(r101Layout->height, 20);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_OLDALE_TOWN)->mapLayout->width, 20);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_OLDALE_TOWN)->mapLayout->height, 20);

    /* Littleroot current: one NORTH connection to Route101, origin (0,-20)
     * (Route101's height 20), per plan lines 536-537. */
    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 1);
    r = &nb->neighbors[0];
    CHECK_EQ_INT(r->direction, CONNECTION_NORTH);
    CHECK_EQ_INT(r->offset, 0);
    CHECK_EQ_INT(r->mapGroup, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_ROUTE101);
    CHECK_EQ_INT(r->worldX, 0);
    CHECK_EQ_INT(r->worldY, -20);
    CHECK_EQ_INT(r->width, 20);
    CHECK_EQ_INT(r->height, 20);

    /* Query into the neighbor: (0,-10) -> Route101 local (0,10). */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 0, -10, &src, &g, &n, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(g, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(n, MAP_NUM_ROUTE101);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 10);
    CHECK_EQ_INT(block, r101Layout->map[10 * 20 + 0]);
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 19, -1, NULL, NULL, NULL, &lx, &ly, NULL));
    CHECK_EQ_INT(lx, 19);
    CHECK_EQ_INT(ly, 19);

    /* Route101 current: NORTH Oldale (0,-20), SOUTH Littleroot (0,+20). */
    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE101);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 2);
    r = &nb->neighbors[0];
    CHECK_EQ_INT(r->direction, CONNECTION_NORTH);
    CHECK_EQ_INT(r->offset, 0);
    CHECK_EQ_INT(r->mapGroup, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_OLDALE_TOWN);
    CHECK_EQ_INT(r->worldX, 0);
    CHECK_EQ_INT(r->worldY, -20);
    r = &nb->neighbors[1];
    CHECK_EQ_INT(r->direction, CONNECTION_SOUTH);
    CHECK_EQ_INT(r->offset, 0);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_LITTLEROOT_TOWN);
    CHECK_EQ_INT(r->worldX, 0);
    CHECK_EQ_INT(r->worldY, 20);

    /* Query into Littleroot's rect from Route101: (0,25) -> local (0,5). */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 0, 25, &src, &g, &n, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(n, MAP_NUM_LITTLEROOT_TOWN);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 5);
    CHECK_EQ_INT(block, litLayout->map[5 * 20 + 0]);
}

/* ------------------------------------------------------ test 9: interior */
static void TestMaysHouseNoConnections(void)
{
    const struct NativeWorldNeighborhood *nb;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;
    u16 block;
    s32 minX, minY, maxX, maxY;

    CHECK_EQ_INT(HeaderFor(MAP_GROUP_INDOOR_LITTLEROOT,
                           MAP_NUM_LITTLEROOT_TOWN_MAYS_HOUSE_1F)->mapLayout->width, 11);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_INDOOR_LITTLEROOT,
                           MAP_NUM_LITTLEROOT_TOWN_MAYS_HOUSE_1F)->mapLayout->height, 9);

    SetLocation(MAP_GROUP_INDOOR_LITTLEROOT, MAP_NUM_LITTLEROOT_TOWN_MAYS_HOUSE_1F);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 0);
    CHECK(NativeWorldNeighborhood_GetBounds(nb, &minX, &minY, &maxX, &maxY));
    CHECK_EQ_INT(minX, 0);
    CHECK_EQ_INT(minY, 0);
    CHECK_EQ_INT(maxX, 11);
    CHECK_EQ_INT(maxY, 9);

    /* Inside the current map. */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 5, 4, &src, &g, &n, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
    CHECK_EQ_INT(g, MAP_GROUP_INDOOR_LITTLEROOT);
    CHECK_EQ_INT(n, MAP_NUM_LITTLEROOT_TOWN_MAYS_HOUSE_1F);
    CHECK_EQ_INT(lx, 5);
    CHECK_EQ_INT(ly, 4);
    CHECK_EQ_INT(block, HeaderFor(MAP_GROUP_INDOOR_LITTLEROOT,
                                  MAP_NUM_LITTLEROOT_TOWN_MAYS_HOUSE_1F)
                            ->mapLayout->map[4 * 11 + 5]);

    /* Outside the union: BORDER (current map's border word). */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 20, 20, &src, NULL, NULL, NULL, NULL, NULL));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_BORDER);
}

/* ------------------------------------------------------- test 10: current */
static void TestQueryInsideCurrent(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct MapLayout *litLayout;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;
    u16 block;

    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    litLayout = nb->currentLayout;
    CHECK(litLayout == HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN)->mapLayout);
    CHECK_EQ_INT(nb->currentWidth, 20);
    CHECK_EQ_INT(nb->currentHeight, 20);

    CHECK(NativeWorldNeighborhood_GetBlock(nb, 10, 10, &src, &g, &n, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
    CHECK_EQ_INT(g, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(n, MAP_NUM_LITTLEROOT_TOWN);
    CHECK_EQ_INT(lx, 10);
    CHECK_EQ_INT(ly, 10);
    CHECK_EQ_INT(block, litLayout->map[10 * 20 + 10]);

    /* Corners: the block grid resolves exactly at the rect edge. */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 0, 0, &src, NULL, NULL, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 0);
    CHECK_EQ_INT(block, litLayout->map[0]);
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 19, 19, &src, NULL, NULL, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_CURRENT);
    CHECK_EQ_INT(lx, 19);
    CHECK_EQ_INT(ly, 19);
    CHECK_EQ_INT(block, litLayout->map[19 * 20 + 19]);
}

/* ------------------------------------------------------- test 11: neighbor */
static void TestQueryInsideNeighbor(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct MapLayout *r101Layout;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;
    u16 block;

    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    r101Layout = HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE101)->mapLayout;

    /* (5,-10) in world blocks = Route101 local (5,10). */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 5, -10, &src, &g, &n, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(g, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(n, MAP_NUM_ROUTE101);
    CHECK_EQ_INT(lx, 5);
    CHECK_EQ_INT(ly, 10);
    CHECK_EQ_INT(block, r101Layout->map[10 * 20 + 5]);

    /* The full Route101 column under Littleroot's north edge: y=-1 local 19. */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 13, -1, &src, NULL, NULL, &lx, &ly, &block));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(lx, 13);
    CHECK_EQ_INT(ly, 19);
    CHECK_EQ_INT(block, r101Layout->map[19 * 20 + 13]);
}

/* -------------------------------------------------- test 14: identity diff */
static void TestIdentityMutationVersion(void)
{
    const struct NativeWorldNeighborhood *nb;
    u32 v1;

    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    v1 = nb->version;
    CHECK_EQ_INT(nb->currentMapGroup, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(nb->currentMapNum, MAP_NUM_LITTLEROOT_TOWN);

    /* Warp to Route101: identity changed -> rebuild + bump. */
    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE101);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->version, v1 + 1);
    CHECK_EQ_INT(nb->currentMapGroup, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(nb->currentMapNum, MAP_NUM_ROUTE101);
    CHECK_EQ_INT(nb->neighborCount, 2);

    /* Connection transition back to Littleroot: same path, another bump. */
    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->version, v1 + 2);
    CHECK_EQ_INT(nb->neighborCount, 1);

    /* Hot path: unchanged identity -> no rebuild, no bump. */
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->version, v1 + 2);

    /* State-restore shape: Invalidate drops the cached identity (Republish /
     * ClearMigratedEntries re-derive every published pointer) -> the next
     * access rebuilds with the SAME identity. */
    NativeWorldNeighborhood_Invalidate();
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->version, v1 + 3);
    CHECK_EQ_INT(nb->currentMapGroup, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(nb->currentMapNum, MAP_NUM_LITTLEROOT_TOWN);
    CHECK_EQ_INT(nb->neighborCount, 1);
}

/* ---------------------------------------------------- test 17: no 2nd hop */
static void TestNoSecondHop(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct NativeWorldNeighborRecord *r;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;

    /* Route101 current: residents = {Route101, Oldale, Littleroot} ONLY.
     * Oldale's own connections (Route103 up, Route102 left) must NOT become
     * residents: the neighborhood is one hop, never recursive. */
    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE101);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 2);
    CHECK_EQ_INT(nb->neighbors[0].mapNum, MAP_NUM_OLDALE_TOWN);
    CHECK_EQ_INT(nb->neighbors[0].direction, CONNECTION_NORTH);
    CHECK_EQ_INT(nb->neighbors[1].mapNum, MAP_NUM_LITTLEROOT_TOWN);
    CHECK_EQ_INT(nb->neighbors[1].direction, CONNECTION_SOUTH);
    r = &nb->neighbors[0];
    CHECK(r->mapNum != MAP_NUM_ROUTE103 && r->mapNum != MAP_NUM_ROUTE102);
    r = &nb->neighbors[1];
    CHECK(r->mapNum != MAP_NUM_ROUTE103 && r->mapNum != MAP_NUM_ROUTE102);

    /* The strip of Oldale visible from Route101 resolves as OLDALE (the map
     * whose rect owns it), never as Oldale's own neighbors. */
    CHECK(NativeWorldNeighborhood_GetBlock(nb, 0, -10, &src, &g, &n, &lx, &ly, NULL));
    CHECK_EQ_INT(src, NATIVE_WORLD_BLOCK_NEIGHBOR);
    CHECK_EQ_INT(g, MAP_GROUP_TOWNS_AND_ROUTES);
    CHECK_EQ_INT(n, MAP_NUM_OLDALE_TOWN);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(ly, 10);

    /* Oldale current: its three one-hop residents, still no second hop. */
    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_OLDALE_TOWN);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 3);
    CHECK_EQ_INT(nb->neighbors[0].direction, CONNECTION_NORTH);
    CHECK_EQ_INT(nb->neighbors[0].mapNum, MAP_NUM_ROUTE103);
    CHECK_EQ_INT(nb->neighbors[1].direction, CONNECTION_SOUTH);
    CHECK_EQ_INT(nb->neighbors[1].mapNum, MAP_NUM_ROUTE101);
    CHECK_EQ_INT(nb->neighbors[2].direction, CONNECTION_WEST);
    CHECK_EQ_INT(nb->neighbors[2].mapNum, MAP_NUM_ROUTE102);
}

/* ----------------------------------------------- test 18: Route124 + dive */
static void TestRoute124DiveExcluded(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct NativeWorldNeighborRecord *r;
    unsigned i;

    /* Route124: 5 raw records (plan §1): down Route126 o0, left Lilycove o10,
     * right Route125 o0, right Mossdeep o40, dive Underwater_Route124 o0. */
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE124)->connections->count, 5);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE124)->mapLayout->width, 80);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE124)->mapLayout->height, 80);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE125)->mapLayout->width, 80);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE125)->mapLayout->height, 40);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_MOSSDEEP_CITY)->mapLayout->width, 80);
    CHECK_EQ_INT(HeaderFor(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_MOSSDEEP_CITY)->mapLayout->height, 40);

    SetLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE124);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
    CHECK_EQ_INT(nb->neighborCount, 4);

    /* Record order = connection declaration order, dive/emerge skipped. */
    r = &nb->neighbors[0];
    CHECK_EQ_INT(r->direction, CONNECTION_SOUTH);
    CHECK_EQ_INT(r->offset, 0);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_ROUTE126);
    CHECK_EQ_INT(r->worldX, 0);
    CHECK_EQ_INT(r->worldY, 80); /* SOUTH: (o, +currentHeight) */
    r = &nb->neighbors[1];
    CHECK_EQ_INT(r->direction, CONNECTION_WEST);
    CHECK_EQ_INT(r->offset, 10);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_LILYCOVE_CITY);
    CHECK_EQ_INT(r->worldX, -80); /* WEST: (-cWidth, o), Lilycove is 80 wide */
    CHECK_EQ_INT(r->worldY, 10);
    r = &nb->neighbors[2];
    CHECK_EQ_INT(r->direction, CONNECTION_EAST);
    CHECK_EQ_INT(r->offset, 0);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_ROUTE125);
    CHECK_EQ_INT(r->worldX, 80); /* EAST: (+currentWidth, o) */
    CHECK_EQ_INT(r->worldY, 0);
    r = &nb->neighbors[3];
    CHECK_EQ_INT(r->direction, CONNECTION_EAST);
    CHECK_EQ_INT(r->offset, 40);
    CHECK_EQ_INT(r->mapNum, MAP_NUM_MOSSDEEP_CITY);
    CHECK_EQ_INT(r->worldX, 80);
    CHECK_EQ_INT(r->worldY, 40);

    /* Underwater_Route124 (the dive target) is never a neighbor. */
    for (i = 0; i < 4; i++)
        CHECK(!(nb->neighbors[i].mapGroup == MAP_GROUP_TOWNS_AND_ROUTES
             && nb->neighbors[i].mapNum == MAP_NUM_UNDERWATER_ROUTE124));
}

/* ------------------------------------------- test 19: data-wide pin sweep */
#define SWEEP_MAX_MAPS 518 /* data pin: map_groups.json total */
#define SWEEP_MAX_RECORDS 8 /* data max is 5 (Route124); headroom, checked */

struct SweepConn
{
    u8 direction;
    s32 offset;
    u8 mapGroup;
    u8 mapNum;
};

struct SweepMap
{
    u8 mapGroup;
    u8 mapNum;
    u8 recordCount;
    struct SweepConn records[SWEEP_MAX_RECORDS];
};

static struct SweepMap sSweep[SWEEP_MAX_MAPS];
static unsigned sSweepCount;

static int FindSweepMap(u8 group, u8 num)
{
    unsigned i;
    for (i = 0; i < sSweepCount; i++)
    {
        if (sSweep[i].mapGroup == group && sSweep[i].mapNum == num)
            return (int)i;
    }
    return -1;
}

static void TestDataWideSweep(void)
{
    /* Per-group map counts from data/maps/map_groups.json "group_order"
     * (34 groups, gMapGroups table order). groups.inc emits bare `gba_ptr`
     * rows with NO per-group count word (plan §1), so the sweep iterates
     * these JSON-derived counts; every identity is cross-checked below via
     * the accessor against the compiled tables. Sum: 518. */
    static const u16 sGroupMapCounts[MAP_GROUPS_COUNT] = {
        57, 5, 5, 6, 7, 8, 9, 7, 7, 14, 8, 17, 10, 23, 13, 15, 15,
        2, 2, 2, 3, 1, 1, 1, 108, 61, 89, 2, 1, 13, 1, 1, 3, 1
    };
    u32 versionTicks;
    unsigned totalRecords = 0;
    unsigned spatialRecords = 0;
    unsigned directionCounts[4] = {0}; /* NORTH, SOUTH, WEST, EAST */
    unsigned dive = 0, emerge = 0;
    unsigned mapsWithAny = 0, mapsWithSpatial = 0, diveEmergeOnly = 0;
    unsigned maxSpatial = 0, maxSameDirection = 0;
    unsigned negativeOffsets = 0;
    unsigned backViolations = 0;
    unsigned overlappingRects = 0;
    struct SweepRect { u8 direction; s32 x, y, w, h; } rects[SWEEP_MAX_RECORDS];
    u8 group;
    u8 num;

    /* The sweep runs after tests 1/9/10/11/14/17/18: start the version
     * expectation at the current (already-bumped) value. */
    versionTicks = NativeWorldNeighborhood_GetState()->version;

    for (group = 0; group < MAP_GROUPS_COUNT; group++)
    {
        for (num = 0; num < sGroupMapCounts[group]; num++)
        {
            const struct NativeWorldNeighborhood *nb;
            const struct MapHeader *header;
            const struct MapConnections *list;
            const struct MapConnection *conn;
            const struct MapLayout *curLayout;
            struct SweepMap *map;
            unsigned spatialInMap = 0;
            unsigned i, k;
            bool32 synthetic;

            header = HeaderFor(group, num);
            CHECK(header != NULL);
            if (header == NULL)
                continue;
            CHECK(sSweepCount < SWEEP_MAX_MAPS);
            if (sSweepCount >= SWEEP_MAX_MAPS)
                return;

            /* The module's build for this identity (each visited once). */
            SetLocation(group, num);
            nb = NativeWorldNeighborhood_GetState();

            /* Raw walk of the compiled <Map>_MapConnections list. */
            map = &sSweep[sSweepCount];
            memset(map, 0, sizeof(*map));
            map->mapGroup = group;
            map->mapNum = num;
            list = header->connections;
            if (list != NULL && list->connections != NULL)
            {
                CHECK(list->count > 0);
                conn = list->connections;
                for (i = 0; i < (unsigned)list->count; i++, conn++)
                {
                    if (i >= SWEEP_MAX_RECORDS)
                        continue; /* count overflow handled by the CHECK below */
                    map->records[map->recordCount].direction = conn->direction;
                    map->records[map->recordCount].offset = conn->offset;
                    map->records[map->recordCount].mapGroup = conn->mapGroup;
                    map->records[map->recordCount].mapNum = conn->mapNum;
                    map->recordCount++;
                    totalRecords++;
                    switch (conn->direction)
                    {
                    case CONNECTION_NORTH: directionCounts[0]++; spatialInMap++; break;
                    case CONNECTION_SOUTH: directionCounts[1]++; spatialInMap++; break;
                    case CONNECTION_WEST:  directionCounts[2]++; spatialInMap++; break;
                    case CONNECTION_EAST:  directionCounts[3]++; spatialInMap++; break;
                    case CONNECTION_DIVE:  dive++; break;
                    case CONNECTION_EMERGE: emerge++; break;
                    default: CHECK(0); /* unknown direction: data corruption */
                    }
                }
                CHECK_EQ_INT(map->recordCount, (unsigned)list->count);
            }

            if (map->recordCount > 0)
                mapsWithAny++;
            if (spatialInMap > 0)
                mapsWithSpatial++;
            if (map->recordCount > 0 && spatialInMap == 0)
                diveEmergeOnly++;
            if (spatialInMap > maxSpatial)
                maxSpatial = spatialInMap;

            synthetic = IsSyntheticLayoutId(header->mapLayoutId);

            /* The module's status must be exactly the fail-closed contract:
             * READY for every non-synthetic map with <= 4 spatial records;
             * DEGRADED for synthetic maps (placeholder grid) and for any
             * impossible 5th spatial connection (module never truncates). */
            if (synthetic || spatialInMap > 4)
            {
                CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_DEGRADED);
                CHECK(nb->currentBlocksSynthetic == synthetic);
                CHECK_EQ_INT(nb->neighborCount, synthetic ? 0 : 4);
            }
            else
            {
                CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_READY);
                CHECK(nb->currentBlocksSynthetic == FALSE);
                CHECK_EQ_INT(nb->neighborCount, spatialInMap);
                versionTicks++;
            }
            CHECK_EQ_INT(nb->version, versionTicks);

            /* Per-record rect math: the module's records must match the
             * independent formula application over the raw records. */
            curLayout = header->mapLayout;
            {
                unsigned rCount = 0;
                for (i = 0; i < map->recordCount; i++)
                {
                    if (IsSpatialDirection(map->records[i].direction))
                    {
                        const struct MapLayout *cLayout =
                            HeaderFor(map->records[i].mapGroup,
                                      map->records[i].mapNum)->mapLayout;
                        struct SweepRect *rect = &rects[rCount];
                        rect->direction = map->records[i].direction;
                        ExpectedRect(map->records[i].direction,
                                     map->records[i].offset,
                                     curLayout->width, curLayout->height,
                                     cLayout->width, cLayout->height,
                                     &rect->x, &rect->y);
                        rect->w = cLayout->width;
                        rect->h = cLayout->height;
                        if (map->records[i].offset < 0)
                            negativeOffsets++;
                        rCount++;
                    }
                }
                CHECK_EQ_INT(rCount, spatialInMap);
                if (rCount > SWEEP_MAX_RECORDS)
                    return;

                if (!synthetic && spatialInMap <= 4)
                {
                    unsigned rIndex = 0;
                    for (i = 0; i < map->recordCount; i++)
                    {
                        const struct SweepConn *raw;
                        const struct NativeWorldNeighborRecord *rec;
                        if (!IsSpatialDirection(map->records[i].direction))
                            continue;
                        raw = &map->records[i];
                        rec = &nb->neighbors[rIndex];
                        CHECK_EQ_INT(rec->direction, raw->direction);
                        CHECK_EQ_INT(rec->offset, raw->offset);
                        CHECK_EQ_INT(rec->mapGroup, raw->mapGroup);
                        CHECK_EQ_INT(rec->mapNum, raw->mapNum);
                        CHECK_EQ_INT(rec->worldX, rects[rIndex].x);
                        CHECK_EQ_INT(rec->worldY, rects[rIndex].y);
                        CHECK_EQ_INT(rec->width, rects[rIndex].w);
                        CHECK_EQ_INT(rec->height, rects[rIndex].h);

                        /* Same-direction duplicates: distinct, non-overlapping
                         * rects (plan §5; the only pairs in the data are
                         * Route111 2xWEST and Route124 2xEAST). */
                        for (k = 0; k < rIndex; k++)
                        {
                            if (rects[k].direction != rects[rIndex].direction)
                                continue; /* not a same-direction pair */
                            if (rects[k].x >= rects[rIndex].x + rects[rIndex].w
                             || rects[rIndex].x >= rects[k].x + rects[k].w
                             || rects[k].y >= rects[rIndex].y + rects[rIndex].h
                             || rects[rIndex].y >= rects[k].y + rects[k].h)
                                continue; /* no overlap */
                            overlappingRects++;
                        }
                        rIndex++;
                    }
                    CHECK_EQ_INT(rIndex, spatialInMap);
                }

                /* Max records in a single direction (same map). */
                for (i = 0; i < map->recordCount; i++)
                {
                    unsigned sameDir = 1;
                    for (k = i + 1; k < map->recordCount; k++)
                    {
                        if (map->records[k].direction == map->records[i].direction
                         && IsSpatialDirection(map->records[i].direction))
                            sameDir++;
                    }
                    if (sameDir > maxSameDirection)
                        maxSameDirection = sameDir;
                }
            }
            sSweepCount++;
        }
    }

    /* Aggregate data pins (plan lines 50-59). The spatial-record aggregate
     * is validated by the independent count below (it is computed over the
     * sweep copy, not the raw walk counters). */
    CHECK_EQ_INT(sSweepCount, SWEEP_MAX_MAPS);
    CHECK_EQ_INT(totalRecords, 148);
    CHECK_EQ_INT(directionCounts[0], 27); /* NORTH */
    CHECK_EQ_INT(directionCounts[1], 27); /* SOUTH */
    CHECK_EQ_INT(directionCounts[2], 40); /* WEST */
    CHECK_EQ_INT(directionCounts[3], 40); /* EAST */
    CHECK_EQ_INT(dive, 7);
    CHECK_EQ_INT(emerge, 7);
    CHECK_EQ_INT(mapsWithAny, 64);
    CHECK_EQ_INT(mapsWithSpatial, 61);
    CHECK_EQ_INT(diveEmergeOnly, 3);
    CHECK_EQ_INT(maxSpatial, 4);
    CHECK_EQ_INT(maxSameDirection, 2);
    CHECK_EQ_INT(negativeOffsets, 18);
    CHECK_EQ_INT(overlappingRects, 0);

    /* Named pins: the two max-spatial maps and the two same-direction pairs. */
    {
        int mi = FindSweepMap(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE124);
        CHECK(mi >= 0);
        if (mi >= 0)
        {
            CHECK_EQ_INT(sSweep[mi].recordCount, 5);
            CHECK_EQ_INT(sSweep[mi].records[4].direction, CONNECTION_DIVE);
        }
        mi = FindSweepMap(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_MAUVILLE_CITY);
        CHECK(mi >= 0);
        if (mi >= 0)
            CHECK_EQ_INT(sSweep[mi].recordCount, 4);
        mi = FindSweepMap(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_ROUTE111);
        CHECK(mi >= 0);
        if (mi >= 0)
        {
            CHECK_EQ_INT(sSweep[mi].recordCount, 3);
            CHECK_EQ_INT(sSweep[mi].records[1].direction, CONNECTION_WEST);
            CHECK_EQ_INT(sSweep[mi].records[2].direction, CONNECTION_WEST);
        }
    }

    /* Back-connection invariant (plan lines 57-59): every A->B spatial record
     * whose B->A exists is opposite-direction with offset -o. */
    {
        unsigned a;
        for (a = 0; a < sSweepCount; a++)
        {
            unsigned i;
            for (i = 0; i < sSweep[a].recordCount; i++)
            {
                const struct SweepConn *r = &sSweep[a].records[i];
                int bIndex;
                unsigned j;
                if (!IsSpatialDirection(r->direction))
                    continue;
                bIndex = FindSweepMap(r->mapGroup, r->mapNum);
                if (bIndex < 0)
                    continue;
                for (j = 0; j < sSweep[bIndex].recordCount; j++)
                {
                    const struct SweepConn *s = &sSweep[bIndex].records[j];
                    if (!IsSpatialDirection(s->direction))
                        continue;
                    if (s->mapGroup != sSweep[a].mapGroup || s->mapNum != sSweep[a].mapNum)
                        continue;
                    if (!OppositeDirections(r->direction, s->direction)
                     || s->offset != -r->offset)
                        backViolations++;
                }
            }
        }
    }
    CHECK_EQ_INT(backViolations, 0);

    /* The 134 spatial records of the 61 spatial maps were validated per-map
     * above; recompute the aggregate for the CHECK (independent count). */
    {
        unsigned a;
        for (a = 0; a < sSweepCount; a++)
        {
            unsigned i;
            for (i = 0; i < sSweep[a].recordCount; i++)
            {
                if (IsSpatialDirection(sSweep[a].records[i].direction))
                    spatialRecords++;
            }
        }
    }
    CHECK_EQ_INT(spatialRecords, 134);
}

/* -------------------------------------------- test 20: synthetic pyramid */
static void TestSyntheticPyramid(void)
{
    const struct NativeWorldNeighborhood *nb;
    const struct MapHeader *header;
    u32 versionBefore;
    enum NativeWorldBlockSource src;
    u8 g, n;
    s32 lx, ly;
    u16 block;
    s32 minX, minY, maxX, maxY;

    header = HeaderFor(MAP_GROUP_SPECIAL_AREA, MAP_NUM_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR);
    CHECK(header != NULL);
    CHECK_EQ_INT(header->mapLayoutId, LAYOUT_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR);

    /* Capture the version with the last READY build (Route124 from test 18). */
    versionBefore = NativeWorldNeighborhood_GetState()->version;

    SetLocation(MAP_GROUP_SPECIAL_AREA, MAP_NUM_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR);
    nb = NativeWorldNeighborhood_GetState();
    CHECK_EQ_INT(nb->status, NATIVE_WORLD_NB_DEGRADED);
    CHECK(nb->currentBlocksSynthetic == TRUE);
    CHECK_EQ_INT(nb->neighborCount, 0);
    CHECK_EQ_INT(nb->version, versionBefore); /* no version bump on DEGRADE */

    /* Every query fails closed, inside the rect AND outside it. */
    CHECK(!NativeWorldNeighborhood_GetBlock(nb, 1, 1, &src, &g, &n, &lx, &ly, &block));
    CHECK(!NativeWorldNeighborhood_GetBlock(nb, 5, -3, &src, &g, &n, &lx, &ly, &block));
    CHECK(!NativeWorldNeighborhood_ResolveBlockForRender(nb, 0, 0, &src, &g, &n,
                                                         &lx, &ly, NULL, &block, NULL));
    CHECK(NativeWorldNeighborhood_GetObjectEventsAt(nb, 0, 0, &n) == NULL);
    CHECK_EQ_INT(n, 0);
    CHECK(!NativeWorldNeighborhood_GetBounds(nb, &minX, &minY, &maxX, &maxY));
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

    printf("== R11-E/F Harness B: native world neighborhood, real data + "
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
    CHECK(Gen3ResourcePack_GetEntryCount(pack) == 5289u); /* 4518 + 569 audio leaves + 202 structural (R12-B/C) */
    CHECK(Gen3ResourceCatalog_Count(catalog) == 5289u); /* + 569 audio leaves + 202 structural (R12-B/C) */

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
    CHECK(info.entryCount == 5289u); /* + 569 audio leaves + 202 structural (R12-B/C) */

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

    TestLittlerootRoute101();   /* 1 */
    TestMaysHouseNoConnections(); /* 9 */
    TestQueryInsideCurrent();   /* 10 */
    TestQueryInsideNeighbor();  /* 11 */
    TestIdentityMutationVersion(); /* 14 */
    TestNoSecondHop();          /* 17 */
    TestRoute124DiveExcluded(); /* 18 */
    TestDataWideSweep();        /* 19 */
    TestSyntheticPyramid();     /* 20 */

    /* Teardown mirroring the production harness's success path: the compat
     * seams' published pointers die with the snapshot, so everything is
     * destroyed only after the last test. */
    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);

    printf("harness B: %u checks, %u failures\n", sChecks, sFailures);
    return sFailures == 0u ? 0 : 1;
}
