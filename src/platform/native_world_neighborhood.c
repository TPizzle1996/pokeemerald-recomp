#if defined(PLATFORM_SDL2) && defined(LINUX64) && LINUX64

#include <stdio.h>

#include "global.h"
#include "fieldmap.h"
#include "overworld.h"
#include "constants/layouts.h"
#include "constants/map_groups.h"
#include "platform/native_world_neighborhood.h"

/*
 * R11-E/F: NativeWorldNeighborhood (see the header for the full contract).
 *
 * The module is a pure function of (serialized location identity + compiled
 * canonical map records + seam-published pointers). It stores pointers and
 * identities only -- never copies of map data -- and its static storage is
 * plain host .data/.bss (no EWRAM_DATA/GBA_DATA attribute), so it is outside
 * every State v5 BuildSlices slice by construction and is never serialized.
 *
 * The module consumes the R11-C tileset + R11-D layout seams (published
 * record fields), the canonical compiled map tables, and
 * Overworld_GetMapHeaderByGroupAndId (the PORTABLE branch). It calls no seam
 * function at runtime: publication is verified by checking the published
 * record fields directly (non-NULL .map/.border/.metatiles), and anything
 * missing fails the build closed to DEGRADED.
 */

static struct NativeWorldNeighborhood sNeighborhood;

/* Cached location identity for the lazy diff. 0xFF pairs never match a real
 * (s8) location, so Init/Invalidate force the next access to rebuild. */
static u8 sCachedMapGroup;
static u8 sCachedMapNum;

static bool32 IsSyntheticGridLayout(u16 mapLayoutId)
{
    /* The battle pyramid floor and the trainer-hill floors generate their
     * grids at runtime (InitBattlePyramidMap / InitTrainerHillMap,
     * fieldmap.c:88-98); their compiled map.bin is a placeholder. The exact
     * layout-id test LoadMapFromWarp uses (overworld.c:859-862): the pyramid
     * constant, or InTrainerHill's floor set (trainer_hill.c:735-749). */
    if (mapLayoutId == LAYOUT_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR)
        return TRUE;
    if (mapLayoutId == LAYOUT_TRAINER_HILL_1F
     || mapLayoutId == LAYOUT_TRAINER_HILL_2F
     || mapLayoutId == LAYOUT_TRAINER_HILL_3F
     || mapLayoutId == LAYOUT_TRAINER_HILL_4F)
        return TRUE;
    return FALSE;
}

/* R11-C/R11-D publication check: the layout record must carry real published
 * .map/.border and a primary tileset with published .metatiles/.metatile-
 * Attributes. (secondaryTileset may be NULL -- the layouts.json "0" case,
 * R11-D report §1 -- and is only touched when a block actually references a
 * secondary metatile, where a NULL secondary is data corruption the GBA path
 * would fault on; the query fails closed instead.) */
static bool32 LayoutPublished(const struct MapLayout *layout)
{
    if (layout == NULL)
        return FALSE;
    if (layout->map == NULL || layout->border == NULL)
        return FALSE;
    if (layout->primaryTileset == NULL)
        return FALSE;
    if (layout->primaryTileset->metatiles == NULL
     || layout->primaryTileset->metatileAttributes == NULL)
        return FALSE;
    return TRUE;
}

static void SetSyntheticFlag(const struct MapHeader *mapHeader)
{
    sNeighborhood.currentBlocksSynthetic = IsSyntheticGridLayout(mapHeader->mapLayoutId);
}

static void BuildNeighborhood(void)
{
    const struct MapHeader *currentMapHeader;
    const struct MapLayout *currentLayout;
    const struct MapConnection *connection;
    s32 i;
    s32 minX, minY, maxX, maxY;

    /* Optimistic fail-closed: nothing from the previous build survives, and
     * version bumps ONLY on a complete READY build at the end. */
    sNeighborhood.neighborCount = 0;
    sNeighborhood.currentBlocksSynthetic = FALSE;
    sNeighborhood.status = NATIVE_WORLD_NB_DEGRADED;

    if (sNeighborhood.currentMapGroup >= MAP_GROUPS_COUNT)
        return; // invalid identity

    currentMapHeader = Overworld_GetMapHeaderByGroupAndId(
        sNeighborhood.currentMapGroup, sNeighborhood.currentMapNum);
    if (currentMapHeader == NULL)
        return;

    currentLayout = currentMapHeader->mapLayout;
    if (!LayoutPublished(currentLayout))
        return;
    if (currentLayout->width <= 0 || currentLayout->height <= 0)
        return; // degenerate dimensions: no real content

    sNeighborhood.currentMapHeader = currentMapHeader;
    sNeighborhood.currentLayout = currentLayout;
    sNeighborhood.currentEvents = currentMapHeader->events;
    sNeighborhood.currentWidth = currentLayout->width;
    sNeighborhood.currentHeight = currentLayout->height;

    SetSyntheticFlag(currentMapHeader);
    if (sNeighborhood.currentBlocksSynthetic)
    {
        /* Runtime-generated grid: the compiled map.bin is a placeholder, never
         * real content. DEGRADE (not READY): GetBlock/ResolveBlockForRender
         * return FALSE for EVERY query, inside and outside the rect, so the
         * caller's runtime RING/GRID path (sBackupMapData) stays authoritative.
         * No version bump. */
        return;
    }

    minX = 0;
    minY = 0;
    maxX = currentLayout->width;
    maxY = currentLayout->height;

    if (currentMapHeader->connections != NULL
     && currentMapHeader->connections->connections != NULL)
    {
        connection = currentMapHeader->connections->connections;
        for (i = 0; i < currentMapHeader->connections->count; i++, connection++)
        {
            struct NativeWorldNeighborRecord *record;
            const struct MapHeader *cHeader;
            const struct MapLayout *cLayout;
            s32 originX, originY;

            if (connection->direction != CONNECTION_NORTH
             && connection->direction != CONNECTION_SOUTH
             && connection->direction != CONNECTION_WEST
             && connection->direction != CONNECTION_EAST)
                continue; // dive/emerge excluded: not spatial adjacency

            cHeader = Overworld_GetMapHeaderByGroupAndId(connection->mapGroup, connection->mapNum);
            if (cHeader == NULL)
                return;
            cLayout = cHeader->mapLayout;
            if (!LayoutPublished(cLayout))
                return;
            if (cLayout->width <= 0 || cLayout->height <= 0)
                return;

            /* World origin per audit §1.4 (Fill*Connection-verified):
             * NORTH (o, -cHeight) SOUTH (o, +height) WEST (-cWidth, o) EAST (+width, o) */
            switch (connection->direction)
            {
            case CONNECTION_NORTH:
                originX = connection->offset;
                originY = -cLayout->height;
                break;
            case CONNECTION_SOUTH:
                originX = connection->offset;
                originY = currentLayout->height;
                break;
            case CONNECTION_WEST:
                originX = -cLayout->width;
                originY = connection->offset;
                break;
            case CONNECTION_EAST:
                originX = currentLayout->width;
                originY = connection->offset;
                break;
            default:
                return; // unreachable (filtered above); fail closed
            }

            if (sNeighborhood.neighborCount == NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS)
            {
                /* Unexpected 5th spatial neighbor: fail closed, NEVER truncate
                 * (§1 decision 6). Impossible in the qualified data (max 4);
                 * a truncated world would serve silently wrong content. */
                fprintf(stderr, "NativeWorldNeighborhood: map (%u,%u) has more than %d "
                        "spatial connections - neighborhood DEGRADED\n",
                        sNeighborhood.currentMapGroup, sNeighborhood.currentMapNum,
                        NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS);
                return;
            }

            record = &sNeighborhood.neighbors[sNeighborhood.neighborCount];
            record->direction = connection->direction;
            record->offset = connection->offset;
            record->worldX = originX;
            record->worldY = originY;
            record->mapGroup = connection->mapGroup;
            record->mapNum = connection->mapNum;
            record->mapHeader = cHeader;
            record->layout = cLayout;
            record->events = cHeader->events;
            record->width = cLayout->width;
            record->height = cLayout->height;
            sNeighborhood.neighborCount++;

            if (originX < minX)
                minX = originX;
            if (originY < minY)
                minY = originY;
            if (originX + cLayout->width > maxX)
                maxX = originX + cLayout->width;
            if (originY + cLayout->height > maxY)
                maxY = originY + cLayout->height;
        }
    }

    sNeighborhood.worldMinX = minX;
    sNeighborhood.worldMinY = minY;
    sNeighborhood.worldMaxX = maxX;
    sNeighborhood.worldMaxY = maxY;

    sNeighborhood.status = NATIVE_WORLD_NB_READY;
    sNeighborhood.version++; // bumps ONLY on a complete READY build
}

static void EnsureCurrent(void)
{
    if (gSaveBlock1Ptr == NULL)
    {
        sNeighborhood.status = NATIVE_WORLD_NB_UNINITIALIZED;
        return;
    }

    sNeighborhood.currentMapGroup = gSaveBlock1Ptr->location.mapGroup;
    sNeighborhood.currentMapNum = gSaveBlock1Ptr->location.mapNum;

    /* Hot path: two u8 compares. All map changes (warps AND connection
     * transitions) funnel through ApplyCurrentWarp -> location. */
    if (sNeighborhood.status == NATIVE_WORLD_NB_READY
     && sNeighborhood.currentMapGroup == sCachedMapGroup
     && sNeighborhood.currentMapNum == sCachedMapNum)
        return;

    BuildNeighborhood();
    sCachedMapGroup = sNeighborhood.currentMapGroup;
    sCachedMapNum = sNeighborhood.currentMapNum;
}

void NativeWorldNeighborhood_Init(void)
{
    memset(&sNeighborhood, 0, sizeof(sNeighborhood));
    sNeighborhood.status = NATIVE_WORLD_NB_UNINITIALIZED;
    sCachedMapGroup = 0xFF;
    sCachedMapNum = 0xFF;
}

void NativeWorldNeighborhood_Shutdown(void)
{
    memset(&sNeighborhood, 0, sizeof(sNeighborhood));
    sNeighborhood.status = NATIVE_WORLD_NB_UNINITIALIZED;
    sCachedMapGroup = 0xFF;
    sCachedMapNum = 0xFF;
}

void NativeWorldNeighborhood_Invalidate(void)
{
    /* Drop the cached identity so the next access rebuilds from scratch --
     * a state restore keeps the identity but re-derives every published
     * pointer (Republish AND ClearMigratedEntries). */
    sCachedMapGroup = 0xFF;
    sCachedMapNum = 0xFF;
    sNeighborhood.status = NATIVE_WORLD_NB_DEGRADED;
    sNeighborhood.neighborCount = 0;
}

const struct NativeWorldNeighborhood *NativeWorldNeighborhood_GetState(void)
{
    EnsureCurrent();
    return &sNeighborhood;
}

/* The fail-closed gate shared by every query: content is served ONLY when
 * the build completed READY on a non-synthetic map. */
static bool32 QueryGate(const struct NativeWorldNeighborhood *nb)
{
    if (nb == NULL)
        return FALSE;
    if (nb->status != NATIVE_WORLD_NB_READY)
        return FALSE;
    if (nb->currentBlocksSynthetic)
        return FALSE;
    return TRUE;
}

/* Declaration-order first-match rect test, exactly as gameplay's
 * GetMapConnectionAtPos/GetIncomingConnection iterate (fieldmap.c:693-698,
 * 758-790): the neighborhood can never disagree with gameplay's notion of
 * "which map is that strip". Returns the record index or -1. */
static s32 FindNeighborAt(const struct NativeWorldNeighborhood *nb, s32 worldX, s32 worldY)
{
    s32 i;
    for (i = 0; i < nb->neighborCount; i++)
    {
        const struct NativeWorldNeighborRecord *record = &nb->neighbors[i];
        if (worldX >= record->worldX && worldX < record->worldX + record->width
         && worldY >= record->worldY && worldY < record->worldY + record->height)
            return i;
    }
    return -1;
}

bool32 NativeWorldNeighborhood_GetBlock(const struct NativeWorldNeighborhood *nb,
                                        s32 worldX, s32 worldY,
                                        enum NativeWorldBlockSource *source,
                                        u8 *mapGroup, u8 *mapNum,
                                        s32 *localX, s32 *localY,
                                        u16 *block)
{
    s32 index;

    if (!QueryGate(nb))
        return FALSE;

    if (worldX >= 0 && worldX < nb->currentWidth
     && worldY >= 0 && worldY < nb->currentHeight)
    {
        if (source)
            *source = NATIVE_WORLD_BLOCK_CURRENT;
        if (mapGroup)
            *mapGroup = nb->currentMapGroup;
        if (mapNum)
            *mapNum = nb->currentMapNum;
        if (localX)
            *localX = worldX;
        if (localY)
            *localY = worldY;
        if (block)
            *block = nb->currentLayout->map[worldY * nb->currentWidth + worldX];
        return TRUE;
    }

    index = FindNeighborAt(nb, worldX, worldY);
    if (index >= 0)
    {
        const struct NativeWorldNeighborRecord *record = &nb->neighbors[index];
        if (source)
            *source = NATIVE_WORLD_BLOCK_NEIGHBOR;
        if (mapGroup)
            *mapGroup = record->mapGroup;
        if (mapNum)
            *mapNum = record->mapNum;
        if (localX)
            *localX = worldX - record->worldX;
        if (localY)
            *localY = worldY - record->worldY;
        if (block)
            *block = record->layout->map[(worldY - record->worldY) * record->width
                                       + (worldX - record->worldX)];
        return TRUE;
    }

    /* BORDER: current map's 2x2 border word with the GBA parity index
     * computed in the BACKUP frame (x_bk = worldX + MAP_OFFSET) -- byte-
     * identical to GetBorderBlockAt at every coordinate the GBA camera can
     * show; the west/north equivalents (camera-invisible on GBA) use the
     * same formula on signed values, a documented forward extension. */
    if (source)
        *source = NATIVE_WORLD_BLOCK_BORDER;
    if (mapGroup)
        *mapGroup = nb->currentMapGroup;
    if (mapNum)
        *mapNum = nb->currentMapNum;
    if (localX)
        *localX = worldX;
    if (localY)
        *localY = worldY;
    if (block)
    {
        s32 xBk = worldX + MAP_OFFSET;
        s32 yBk = worldY + MAP_OFFSET;
        s32 i = ((xBk + 1) & 1) + ((yBk + 1) & 1) * 2;
        *block = nb->currentLayout->border[i] | MAPGRID_IMPASSABLE;
    }
    return TRUE;
}

/* DrawMetatileAt's owner-tileset + attribute resolution (field_camera.c:238-
 * 255), per OWNING map: ids < 512 use the primary tileset, ids >= 512 the
 * secondary (indexed minus 512). Returns TRUE when the resolution succeeded. */
static bool32 ResolveOwnerTileset(const struct MapLayout *layout, u16 metatileId,
                                  const u16 **metatiles, u8 *layerType)
{
    const struct Tileset *tileset;
    u16 id;

    if (metatileId < NUM_METATILES_IN_PRIMARY)
    {
        tileset = layout->primaryTileset;
        id = metatileId;
    }
    else
    {
        tileset = layout->secondaryTileset;
        id = metatileId - NUM_METATILES_IN_PRIMARY;
    }

    if (tileset == NULL)
        return FALSE; // data corruption: a block references a missing secondary tileset
    if (tileset->metatiles == NULL || tileset->metatileAttributes == NULL)
        return FALSE; // tileset not published
    if (id >= NUM_METATILES_IN_PRIMARY)
        return FALSE; // out of range on this tileset

    if (metatiles)
        *metatiles = tileset->metatiles;
    if (layerType)
        *layerType = UNPACK_LAYER_TYPE(tileset->metatileAttributes[id]);
    return TRUE;
}

bool32 NativeWorldNeighborhood_ResolveBlockForRender(const struct NativeWorldNeighborhood *nb,
                                                     s32 worldX, s32 worldY,
                                                     enum NativeWorldBlockSource *source,
                                                     u8 *mapGroup, u8 *mapNum,
                                                     s32 *localX, s32 *localY,
                                                     const struct MapLayout **layout,
                                                     u16 *metatileId,
                                                     u8 *layerType)
{
    const struct MapLayout *ownerLayout;
    enum NativeWorldBlockSource src;
    u16 block;
    u16 id;
    u8 attrLayer;

    if (!NativeWorldNeighborhood_GetBlock(nb, worldX, worldY, &src, mapGroup, mapNum,
                                          localX, localY, &block))
        return FALSE;
    if (source)
        *source = src;

    /* BORDER cells belong to the current map (its border word + its tilesets),
     * matching DrawMetatileAt. Neighbor cells belong to the neighbor. */
    if (src == NATIVE_WORLD_BLOCK_NEIGHBOR)
    {
        s32 index = FindNeighborAt(nb, worldX, worldY);
        ownerLayout = nb->neighbors[index].layout;
    }
    else
    {
        ownerLayout = nb->currentLayout;
    }
    if (layout)
        *layout = ownerLayout;

    id = UNPACK_METATILE(block);
    if (id > NUM_METATILES_TOTAL)
        id = 0; // DrawMetatileAt's clamp (field_camera.c:243-244)
    if (metatileId)
        *metatileId = id;

    if (!ResolveOwnerTileset(ownerLayout, id, NULL, &attrLayer))
        return FALSE;
    if (layerType)
        *layerType = attrLayer;
    return TRUE;
}

u16 NativeWorldNeighborhood_MetatileTileEntryForLayer(u8 bg, const u16 *tiles,
                                                      u8 layerType, u8 quadrant)
{
    /* Exact split/covered/normal BG3/BG2/BG1 mapping from field_camera.c's
     * DrawMetatile -- the byte rule of native_overworld_renderer.c's static
     * MetatileTileEntryForLayer, exposed for the future renderer. */
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

const struct MapEvents *NativeWorldNeighborhood_GetObjectEventsAt(
    const struct NativeWorldNeighborhood *nb, s32 worldX, s32 worldY,
    u8 *objectEventCount)
{
    s32 index;

    if (!QueryGate(nb))
    {
        if (objectEventCount)
            *objectEventCount = 0;
        return NULL;
    }

    if (worldX >= 0 && worldX < nb->currentWidth
     && worldY >= 0 && worldY < nb->currentHeight)
    {
        if (objectEventCount)
            *objectEventCount = nb->currentEvents->objectEventCount;
        return nb->currentEvents;
    }

    index = FindNeighborAt(nb, worldX, worldY);
    if (index >= 0)
    {
        const struct NativeWorldNeighborRecord *record = &nb->neighbors[index];
        if (objectEventCount)
            *objectEventCount = record->events->objectEventCount;
        return record->events;
    }

    if (objectEventCount)
        *objectEventCount = 0;
    return NULL;
}

bool32 NativeWorldNeighborhood_GetBounds(const struct NativeWorldNeighborhood *nb,
                                         s32 *minX, s32 *minY, s32 *maxX, s32 *maxY)
{
    if (!QueryGate(nb))
        return FALSE;
    if (minX)
        *minX = nb->worldMinX;
    if (minY)
        *minY = nb->worldMinY;
    if (maxX)
        *maxX = nb->worldMaxX;
    if (maxY)
        *maxY = nb->worldMaxY;
    return TRUE;
}

#endif // PLATFORM_SDL2 && LINUX64
