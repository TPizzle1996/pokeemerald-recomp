#ifndef GUARD_PLATFORM_NATIVE_WORLD_NEIGHBORHOOD_H
#define GUARD_PLATFORM_NATIVE_WORLD_NEIGHBORHOOD_H

#include "global.h"
#include "fieldmap.h"

/*
 * NativeWorldNeighborhood (R11-E/F)
 * --------------------------------
 *
 * The native world-coordinate neighborhood: the current map plus its ONE-HOP
 * directly connected neighbors, each placed at its computed world origin in
 * block units (1 map block = 16 px = 1 pos.x/y unit = 1 connection-offset
 * unit). Pure infrastructure for renderer/query-facing composition -- no
 * gameplay change, no zoom, no second resource system.
 *
 * The current map sits at world origin (0,0). A connection with offset o
 * places the neighbor at (audit-verified against Fill*Connection):
 *
 *     NORTH (o, -cHeight)   SOUTH (o, +height)   WEST (-cWidth, o)   EAST (+width, o)
 *
 *   (active map at (0,0), cWidth/cHeight = the NEIGHBOR's layout dimensions).
 *
 * Lifecycle
 * ---------
 *   - Init()        once at content hydration, AFTER the compat seams
 *                   publish (desktop_game_content.c, guarded NATIVE_LINUX).
 *   - Invalidate()  from the state-restore path right after
 *                   EmeraldResourceCompat_Republish / ClearMigratedEntries
 *                   (native_state.c, guarded PLATFORM_SDL2 && NATIVE_LINUX):
 *                   a restore keeps the map identity but re-derives every
 *                   published pointer, so the cached records must be dropped
 *                   and rebuilt on next access.
 *   - Lazy          every public entry (GetState and anything built on it)
 *                   starts with EnsureCurrent(), which diffs
 *                   gSaveBlock1Ptr->location.{mapGroup,mapNum} and rebuilds
 *                   on change. All map changes -- warps AND connection
 *                   transitions -- funnel through ApplyCurrentWarp -> location.
 *
 * Storage: plain host .data/.bss (no EWRAM_DATA/GBA_DATA attribute) -> outside
 * every State v5 BuildSlices slice by construction; the module is never
 * serialized and never owns memory (records hold pointers + identities only).
 *
 * Fail-closed contract
 * --------------------
 * The neighborhood serves content ONLY when status == READY and the current
 * map is not a synthetic-grid map (battle pyramid floor / trainer-hill
 * floors: their compiled map.bin is a placeholder, the runtime RING/GRID in
 * sBackupMapData is the only truth). Otherwise GetBlock /
 * ResolveBlockForRender / GetObjectEventsAt / GetBounds return FALSE / NULL
 * and the caller MUST fall back to the legacy RING/GRID/border path.
 */

// Data-pinned capacity: the qualified Emerald data has at most 4 spatial
// connections per map (Route124, Mauville). The builder NEVER truncates an
// unexpected fifth: it fails to DEGRADED with a diagnostic instead.
#define NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS 4    /* data-pinned: max spatial connections per map */
#define NATIVE_WORLD_NEIGHBORHOOD_MAX_RESIDENT_MAPS \
    (1 + NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS)    /* 1 current + 4 neighbors (bounds/doc only) */

enum NativeWorldNeighborhoodStatus
{
    NATIVE_WORLD_NB_UNINITIALIZED,  /* gSaveBlock1Ptr NULL / never initialized */
    NATIVE_WORLD_NB_READY,          /* identity resolved, all layouts published */
    NATIVE_WORLD_NB_DEGRADED,       /* identity invalid OR a layout/tileset not published
                                       (or synthetic-map placeholder state) - fail closed */
};

enum NativeWorldBlockSource
{
    NATIVE_WORLD_BLOCK_CURRENT,   /* inside current map rect */
    NATIVE_WORLD_BLOCK_NEIGHBOR,  /* inside a neighbor rect */
    NATIVE_WORLD_BLOCK_BORDER,    /* outside the union - current map's border word */
};

struct NativeWorldNeighborRecord
{
    u8 direction;                    /* CONNECTION_NORTH/SOUTH/WEST/EAST only */
    s32 offset;                      /* connection offset, block units */
    s32 worldX;                      /* neighbor origin in world blocks */
    s32 worldY;
    u8 mapGroup;
    u8 mapNum;
    const struct MapHeader *mapHeader;   /* canonical compiled record */
    const struct MapLayout *layout;      /* == mapHeader->mapLayout (seam record) */
    const struct MapEvents *events;      /* == mapHeader->events (view only) */
    s32 width;                           /* layout->width  (blocks) */
    s32 height;                          /* layout->height (blocks) */
};

struct NativeWorldNeighborhood
{
    u32 version;                    /* bumped ONLY on a complete READY rebuild */
    enum NativeWorldNeighborhoodStatus status;
    u8 currentMapGroup;
    u8 currentMapNum;
    const struct MapHeader *currentMapHeader;
    const struct MapLayout *currentLayout;
    const struct MapEvents *currentEvents;
    s32 currentWidth;
    s32 currentHeight;
    bool32 currentBlocksSynthetic;  /* battle pyramid floor / trainer hill: runtime-
                                       generated grid. The neighborhood DEGRADES and
                                       refuses EVERY query for these maps (the compiled
                                       map.bin is a placeholder, never real content);
                                       the flag records the reason. */
    s32 worldMinX, worldMinY;       /* union bounds incl. neighbors, block units */
    s32 worldMaxX, worldMaxY;
    u8 neighborCount;               /* 0..NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS */
    struct NativeWorldNeighborRecord neighbors[NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS];
};

/* Zero + UNINITIALIZED. Call once at content hydration AFTER the compat
 * seams publish (next to EmeraldResourceCompat_TryInitialize). */
void NativeWorldNeighborhood_Init(void);

/* Zero + UNINITIALIZED (tests/exit). */
void NativeWorldNeighborhood_Shutdown(void);

/* Force a rebuild on the next access. Called from the state-restore path
 * (covers Republish AND the ClearMigratedEntries fail-closed branch). */
void NativeWorldNeighborhood_Invalidate(void);

/* EnsureCurrent() (lazy rebuild by location identity) + return the state.
 * Callers hold the returned pointer for the frame; query functions below
 * are pure over it and never rebuild. */
const struct NativeWorldNeighborhood *NativeWorldNeighborhood_GetState(void);

/* Query: world block coordinate -> source + owning map + local coords + block word.
   Returns FALSE when status != READY or currentBlocksSynthetic (caller MUST
   fall back to the legacy RING/GRID/border path - this is the fail-closed
   contract; on synthetic maps the runtime grid is the only truth). Out
   params may be NULL. */
bool32 NativeWorldNeighborhood_GetBlock(const struct NativeWorldNeighborhood *nb,
                                        s32 worldX, s32 worldY,
                                        enum NativeWorldBlockSource *source,
                                        u8 *mapGroup, u8 *mapNum,
                                        s32 *localX, s32 *localY,
                                        u16 *block);

/* R11-F renderer seam: everything DrawMetatileAt needs for one world block,
   resolved against the OWNING map's tilesets. Pure function; no ring, no
   snapshot. Out params may be NULL. Returns FALSE on the same fail-closed
   conditions as GetBlock (including synthetic maps - placeholder blockdata
   is never served). */
bool32 NativeWorldNeighborhood_ResolveBlockForRender(const struct NativeWorldNeighborhood *nb,
                                                     s32 worldX, s32 worldY,
                                                     enum NativeWorldBlockSource *source,
                                                     u8 *mapGroup, u8 *mapNum,
                                                     s32 *localX, s32 *localY,
                                                     const struct MapLayout **layout,
                                                     u16 *metatileId,
                                                     u8 *layerType);

/* The field_camera.c DrawMetatile layer->BG tile-entry rule (field_camera.c:257-),
   exposed for the future renderer; byte rule of native_overworld_renderer.c's
   static MetatileTileEntryForLayer. bg is the BG number (1/2/3). */
u16 NativeWorldNeighborhood_MetatileTileEntryForLayer(u8 bg, const u16 *tiles,
                                                      u8 layerType, u8 quadrant);

/* Object-event definition views (decision 10): owning map's events at a world
   block; NULL/count 0 when BORDER or not READY. */
const struct MapEvents *NativeWorldNeighborhood_GetObjectEventsAt(
    const struct NativeWorldNeighborhood *nb, s32 worldX, s32 worldY,
    u8 *objectEventCount);

/* World-rect bounds of the union (block units); (0,0,w,h) for current-only maps. */
bool32 NativeWorldNeighborhood_GetBounds(const struct NativeWorldNeighborhood *nb,
                                         s32 *minX, s32 *minY, s32 *maxX, s32 *maxY);

#endif // GUARD_PLATFORM_NATIVE_WORLD_NEIGHBORHOOD_H
