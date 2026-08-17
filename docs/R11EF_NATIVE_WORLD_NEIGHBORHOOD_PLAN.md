# R11-E/F: NativeWorldNeighborhood + connected-map composition — plan

Stage: R11-E/F of the overworld resource migration (R11_OVERWORLD_RESOURCE_AUDIT
§1.3/§1.4/§3): build the native world-coordinate neighborhood — the current map
plus its ONE-HOP directly connected neighbors at their computed world origins —
as renderer-facing / query-facing infrastructure. No gameplay change, no zoom,
no second resource system.

Method: mirror the R11-C/R11-D discipline — a new native-only module in
`src/platform/` (automatically excluded from the GBA build by Makefile:230's
`NATIVE_ONLY_C_SRCS` filter, automatically collected by Makefile_pc's wildcard,
Makefile_pc:240), guarded `#if defined(PLATFORM_SDL2) && defined(LINUX64) &&
LINUX64` like native_overworld_renderer.c. It CONSUMES the R11-C tileset +
R11-D layout seams and the canonical compiled map tables; it owns nothing.

---

## 0. Verified source-tree evidence (everything below cites this)

- **World unit = one map block = 16 px = 1 metatile.** `gFieldCamera.x %= 16`
  and `AddCameraTileOffset(..., deltaX * 2, ...)` (field_camera.c:420-435):
  one pos.x step = 16 px = 2 ring tiles. The native renderer's world space is
  the same: "metatile (mx,my) occupies world pixels [mx*16,(mx+1)*16)",
  logical origin = cameraMapX*16 + cameraX" (native_overworld_snapshot.h:40-47).
  `pos.x/y`, `MapLayout.width/height`, and connection offsets are ALL in block
  units (MapGridGetMetatileIdAt samples the grid at (x, y) directly,
  fieldmap.c:365-375).
- **Composition formulas** (audit §1.4, verified against Fill*Connection,
  fieldmap.c:190-340): active map at origin (0,0), connection offset o →
  NORTH (o, −cHeight), SOUTH (o, +height), WEST (−cWidth, o),
  EAST (+width, o). The MAP_OFFSET (7, fieldmap.h:18) backup-grid apron
  (InitBackupMapLayoutData, fieldmap.c:119-131) exists only for the GBA grid;
  the neighborhood holds FULL maps, no expansion.
- **Identity source**: every map change (warp AND connection transition)
  funnels through `ApplyCurrentWarp()` → `gSaveBlock1Ptr->location` =
  sWarpDestination (overworld.c:625-628, 666-669); `LoadMapFromCameraTransition`
  (overworld.c:808-850) and `WarpIntoMap` both end in `LoadCurrentMapData()`
  (overworld.c:610-617) which re-reads location. One lazy diff on
  `gSaveBlock1Ptr->location.{mapGroup,mapNum}` catches every transition.
- **Canonical map tables**: `gMapGroups` is a 34-entry `GbaAddr` table
  (MAP_GROUPS_COUNT, constants/map_groups.h:594; groups.inc = bare `gba_ptr`
  rows, NO per-group count word); `Overworld_GetMapHeaderByGroupAndId`
  (overworld.c:595-603, PORTABLE branch) = `HostResolveGbaTableEntry(
  HostResolveGbaAddr(gMapGroups[group]), num)`; `HostResolveGbaAddr` on linux64
  is an identity conversion (host_memory.c:308-326, non-PIE < 4 GiB link).
  Map headers are emitted with `host_ptr` fields (data/maps/LittlerootTown/
  header.inc), so `mapHeader->mapLayout/events/connections` are REAL host
  pointers at native runtime; the layout records they point at are the seam-TU
  definitions from layout_native.generated.h (R11-D §7).
- **Connections data**: 518 maps; **148 connection records total = 134
  spatial (40 left + 40 right + 27 up + 27 down) + 7 dive + 7 emerge**
  (CONNECTION_DIVE/EMERGE, constants/global.h:153-154, non-spatial). 64 maps
  have ≥1 connection of any kind (61 with ≥1 spatial; 3 dive/emerge-only),
  max 4 spatial per map (Route124, Mauville), max 2 in one direction
  (Route111 2×WEST: Route113 offset 0 + Route112 offset 20; Route124 2×EAST:
  Route125 offset 0 + Mossdeep offset 40), 18 negative offsets (e.g.
  Route122→Route123 south offset −100). **Back-connection invariant:
  0 violations across all 134 spatial records** — every A→B (dir, o) whose
  B→A exists is opposite-dir with offset −o (data sweep, this plan).
- **Border**: `GetBorderBlockAt` (fieldmap.c:51-60): 2×2 words, index
  `i = ((x+1)&1) + ((y+1)&1)*2`, block = `border[i] | MAPGRID_IMPASSABLE`,
  computed in the MAP_OFFSET-expanded backup frame; `DrawMetatileAt` clamps
  metatile ids > NUM_METATILES_TOTAL to 0 (field_camera.c:243-244). The
  current native renderer already classifies world pixels as ring / grid /
  connection-strip / border (NATIVE_OVERWORLD_PROVIDER_*, native_overworld_
  renderer.h:142-144; ResolveBgTileEntries, native_overworld_renderer.c:798-833
  → GetMetatileTileEntryFromSnapshot :744-763).
- **Tileset note (strips)**: the GBA apron draws neighbor blocks with the
  ACTIVE map's tilesets (DrawMetatileAt uses `mapLayout->primaryTileset/
  secondaryTileset` of the CURRENT map, field_camera.c:238-255). The
  neighborhood resolves each map's blocks with its OWNER's tilesets; the
  apron-region reconciliation is R11-I/P's job (decision 9/§8).
- **Serialized slices (State v5)**: BuildSlices = GAME_BSS, REGISTERS,
  VIDEO_MEMORY, FLASH, EWRAM, IWRAM, COMMON, GAME_DATA, FRAMEBUFFER, task/
  sprite/battle sidecars, RTC (native_state.c:323-348). Game-owned ranges come
  from the linker script; host .data/.bss is outside every slice. Section
  attributes: `EWRAM_DATA`/`GBA_DATA` (gba/defines.h:11-13) are the ONLY way
  into the gba sections — the neighborhood module uses neither.
- **Seam lifecycle**: umbrella TryInitialize publishes all families including
  layout (emerald_trainer_native_compat.c:862-906); Republish is idempotent
  + allocation-free, called after state load (native_state.c:2902-2910,
  guarded `#if PLATFORM_SDL2 && NATIVE_LINUX`); runtime snapshot registration
  + TryInitialize at content hydration (desktop_game_content.c:745-755,
  guarded `#if NATIVE_LINUX`). ClearMigratedEntries NULLs layout .map/.border
  (R11-D §7).
- **Renderer seam precedent**: `NativeOverworld_CaptureSnapshot` /
  `DrawMapFrameWithMetaEx` (desktop_video.c:295-330;
  native_overworld_renderer.c:1273-1350) — the world-coordinate resolution
  this stage must NOT change; `DrawExpandedComposite` and NativeViewport
  scaleQ8 are the experimental zoom workstream (audit §1.5) that R11-E/F
  leaves untouched.

---

## 1. The 13 design decisions (explicit)

1. **Owner/lifetime**: the module itself — one `static struct
   NativeWorldNeighborhood sNeighborhood` in
   src/platform/native_world_neighborhood.c. No heap allocation, no task, no
   global subsystem. Explicit Init/Shutdown + lazy self-maintenance.

2. **Rebuild triggers**: (a) LAZY — every public entry point starts with
   `EnsureCurrent()` which diffs `gSaveBlock1Ptr->location.{mapGroup,mapNum}`
   against the cached identity and rebuilds on change. This covers map load,
   warp, and connection transition with ZERO game-code edits (all three
   funnel through ApplyCurrentWarp → location, evidence above). (b) EXPLICIT
   — `Invalidate()` called from the state-restore path right next to
   `EmeraldResourceCompat_Republish` (native_state.c:2902 block) because a
   restore keeps the identity but re-derives every published pointer;
   Invalidate also re-verifies publication (covers the ClearMigratedEntries
   fail-closed branch). (c) Init once at content hydration after
   TryInitialize (desktop_game_content.c:755).

3. **Pointers + identities, no copies**: the neighborhood stores the map
   identity (group/num, the lazy-diff key) and CONST POINTERS to the canonical
   compiled records (`MapHeader*`, `MapLayout*`, `MapEvents*` — host-width,
   never freed). Payload pointers (`layout->map`, `->border`, tileset
   members) are dereferenced THROUGH the record at query time, so a
   post-restore Republish is automatically visible; nothing is copied into
   the neighborhood and the neighborhood owns no memory.

4. **Seam resolution**: neighbors resolve via
   `Overworld_GetMapHeaderByGroupAndId` (the canonical accessor, PORTABLE
   branch) → header->mapLayout = the seam-defined record whose .map/.border
   were published by EmeraldLayoutCompat (R11-D), tilesets by
   EmeraldTilesetCompat (R11-C). At build time the module VERIFIES every
   resident layout has non-NULL published .map/.border (and non-NULL
   tilesets with .metatiles/.metatileAttributes); any failure → DEGRADED
   (fail closed, nothing cached as usable). It never reads unpublished
   records and never touches the snapshot/session machinery itself.

5. **Duplicate/cyclic connections**: one neighbor record per connection
   record, no merging. Cycles (Littleroot↔Route101) are just two records
   when the other map is current. Same-direction duplicates (Route111 2×WEST,
   Route124 2×EAST) become two records with distinct offsets; verified
   non-overlapping in the qualified data (offsets tile exactly: Route111's
   0/20 against Route113's height 20; Route124's 0/40 against Route125's
   height 40) — the data-wide pin test (§11 tests) asserts this stays true.

6. **Multiple connections per direction**: max 2 in one direction, max 4
   spatial neighbors per map (both data-pinned, §11). Capacity:
   `NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS 4` — the `neighbors[]` array
   bound — and `NATIVE_WORLD_NEIGHBORHOOD_MAX_RESIDENT_MAPS 5`
   (`1 + MAX_NEIGHBORS`, used for bounds arithmetic and documentation only;
   the neighborhood holds no resident-map array). The builder NEVER
   truncates: an unexpected fifth spatial connection (impossible in the
   qualified data) fails the build to DEGRADED with a diagnostic instead of
   dropping a record; the pin test proves current Emerald data never
   exceeds four.

7. **Asymmetric dimensions / positive+negative offsets**: the formulas are
   dimension- and sign-agnostic. Negative offsets shift the neighbor left/up
   (18 in data; Route113's back-connection to Route112 uses −60). Neighbor
   rects may extend beyond the current map's span (Route113 is 100 blocks
   wide west of the 40-wide Route111) — queries are per-rect, no clamping.

8. **World-coordinate queries outside the active map**: rect test per
   neighbor (declaration order, first match — the same order
   GetMapConnectionAtPos/GetIncomingConnection select in, fieldmap.c:693-698,
   758-790, so the neighborhood never disagrees with gameplay's notion of
   "which map is that strip"); outside the union → BORDER (current map's
   border word, §8). No VOID: on GBA the border repeats indefinitely.

9. **Border vs neighbor content**: the source enum {CURRENT, NEIGHBOR,
   BORDER} is the distinction. BORDER = current map's 2×2 border word |
   MAPGRID_IMPASSABLE with the GBA's parity index computed in the BACKUP
   frame (x_bk = x_local + MAP_OFFSET) — byte-identical to the GBA at every
   coordinate the GBA camera can show (east/south/off-corner apron and
   beyond; the west/north equivalents are camera-invisible on GBA and use
   the same formula on signed values, a documented forward extension).
   Neighbor rects are a SUPERSET of the GBA apron strips (full maps vs
   MAP_OFFSET-deep strips) — the point of R11; apron-exact visual parity is
   owned by R11-I/P, and the 240×160 presentation path is untouched in E/F.

10. **Object-event definitions**: each resident record carries `const struct
    MapEvents*` + the current map's too; accessor
    `NativeWorldNeighborhood_GetObjectEventsAt()` returns the OWNING map's
    `objectEvents` (ObjectEventTemplate*) + `objectEventCount` — views only.
    No template copying, no `LoadObjEventTemplatesFromHeader`, no AI, no
    neighbor simulation. Future rendering/query resolves graphics through
    the R11-B seam `GetObjectEventGraphicsInfo(graphicsId)`
    (event_object_movement.c:1917-1930) exactly as gameplay does. Script
    fields stay GbaAddr + `ObjectEventTemplate_GetScript` untouched.

11. **Derivability / State v5 exclusion**: the neighborhood is a pure
    function of (serialized location identity + compiled canonical records +
    seam-published pointers). Its static storage has no `EWRAM_DATA`/`GBA_DATA`
    attribute → it lands in host .data/.bss → outside EVERY BuildSlices slice
    by construction (same argument as R6's trainer tables, native_state.c
    restore comment). Three-layer proof in §10: section attribution, range
    intersection, and behavioral cross-restart. The two lifecycle-hook edits
    change no slice and no serialized byte.

12. **Renderer consumption later**: E/F defines the seam and proves it
    offscreen; the integration point is `ResolveBgTileEntries`'s fallback
    branch (native_overworld_renderer.c:832-834) — deferred to R11-G/H/J.
    Zoom stays OFF: native_overworld_viewport.c, desktop_video.c, and
    native_overworld_renderer.c are NOT modified in E/F (checkpoint-gate:
    `git diff` on them must be empty); the viewport remains 1.0x/240×160.

13. **E vs F split**: ONE module, TWO checkpoints. R11-E = model + builder +
    lifecycle + coordinate math + query API + all non-render tests
    (checkpoints 1-6). R11-F = the renderer-facing seam +
    world-rect offscreen proof (checkpoint 7). Report as two stages inside
    this one plan.

---

## 2. Module placement + build integration

- **New**: `src/platform/native_world_neighborhood.c` +
  `include/platform/native_world_neighborhood.h`.
- GBA build: unaffected by construction — Makefile:230 filters every
  src/platform/*.c (except host_memory.c) out of the GBA C_SRCS (replaced by
  src/stub.c). No Makefile edit on either side (Makefile_pc:240 wildcard
  picks the new file up).
- The TU opens with the house guard `#if defined(PLATFORM_SDL2) &&
  defined(LINUX64) && LINUX64` (empty TU otherwise, per
  native_overworld_renderer.c).
- Includes: global.h, fieldmap.h (MAP_OFFSET, MapGrid accessor masks),
  overworld.h (Overworld_GetMapHeaderByGroupAndId), constants/map_groups.h
  (MAP_GROUPS_COUNT), emerald/resources/emerald_layout_compat.h +
  emerald_tileset_compat.h ONLY for the publication-verification contract
  (the module dereferences published record fields; it calls no seam
  function at runtime).
- No new resource-family code, no gen3-resource changes, no pack changes,
  no elf_manifest changes, no ownership/inventory changes (the isolation
  battery must stay byte-green: the module adds no payload bytes to the
  binary — pointers only).

## 3. Data model

```c
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
```

## 4. API (include/platform/native_world_neighborhood.h)

```c
void  NativeWorldNeighborhood_Init(void);        /* zero + UNINITIALIZED */
void  NativeWorldNeighborhood_Shutdown(void);    /* zero + UNINITIALIZED (tests/exit) */
void  NativeWorldNeighborhood_Invalidate(void);  /* force rebuild on next access
                                                    (state-restore hook; covers
                                                    Republish AND ClearMigratedEntries) */
const struct NativeWorldNeighborhood *NativeWorldNeighborhood_GetState(void); /* EnsureCurrent + return */

/* Query: world block coordinate -> source + owning map + local coords + block word.
   Returns FALSE when status != READY or currentBlocksSynthetic (caller MUST
   fall back to the legacy RING/GRID/border path - this is the fail-closed
   contract; on synthetic maps the runtime grid is the only truth). */
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
   static MetatileTileEntryForLayer. */
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
```

Every function that reads the state calls `EnsureCurrent()` first (lazy
rebuild, decision 2a) — callers never see a stale identity.

## 5. Builder: EnsureCurrent()

```
EnsureCurrent():
  nb = &sNeighborhood
  if gSaveBlock1Ptr == NULL: status = UNINITIALIZED; return
  identity = gSaveBlock1Ptr->location.{mapGroup,mapNum}
  if identity == cached && status == READY: return            // hot path: two u8 compares
  build:
    neighborCount = 0, currentBlocksSynthetic = FALSE,
    status = DEGRADED (optimistic fail-closed)   // version bumps ONLY on a complete READY build
    if mapGroup >= MAP_GROUPS_COUNT: return                    // invalid identity
    currentMapHeader = Overworld_GetMapHeaderByGroupAndId(group, num)  // PORTABLE branch
    if !currentMapHeader: return
    currentLayout = currentMapHeader->mapLayout
    if !currentLayout || currentLayout->map == NULL || currentLayout->border == NULL: return
    if !currentLayout->primaryTileset || !primaryTileset->metatiles: return   // R11-C published?
    // (secondaryTileset may be NULL - the layouts.json "0" case, R11-D report §1)
    currentBlocksSynthetic =
        (currentMapHeader->mapLayoutId == LAYOUT_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR)
     || InTrainerHillMapLayout(currentMapHeader->mapLayoutId);   // trainer hill layout id
    if currentBlocksSynthetic:
        // Runtime-generated grid: the compiled map.bin is a placeholder, never
        // real content. DEGRADE (not READY): GetBlock/ResolveBlockForRender
        // return FALSE for EVERY query, inside and outside the rect, so the
        // caller's runtime RING/GRID path (sBackupMapData) stays authoritative.
        // No version bump. (§8, §11 test 20)
        return
    for each connection in currentMapHeader->connections (count from
        currentMapHeader->connections->count):
      if direction not in {N,S,W,E}: continue                    // dive/emerge excluded
      cHeader = Overworld_GetMapHeaderByGroupAndId(c.mapGroup, c.mapNum)
      cLayout = cHeader->mapLayout
      if !cHeader || !cLayout || cLayout->map == NULL || cLayout->border == NULL: return
      if !cLayout->primaryTileset || !cLayout->primaryTileset->metatiles: return
      origin = per formula (§6)
      if neighborCount == NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS:
          // unexpected 5th spatial neighbor: fail closed, NEVER truncate (§1 decision 6)
          status = DEGRADED; diagnostic; return
      store record {direction, offset, origin,
               group, num, cHeader, cLayout, cHeader->events, width, height}
      neighborCount++
    union bounds = min/max over current rect + neighbor rects
    status = READY
    version++                            // bumps ONLY on a complete READY build
```

Notes:
- mapNum is NOT bounds-checked: the compiled group tables carry no per-group
  count (groups.inc is bare `gba_ptr` rows — verified), and
  Overworld_GetMapHeaderByGroupAndId itself is unguarded — the neighborhood
  inherits the game's invariant (location and connection targets are always
  valid ids; the data-wide invariant test cross-validates all 134 spatial
  connection targets — dive/emerge records never resolve a header).
- `InTrainerHillMapLayout` = mapLayoutId comparison against the trainer-hill
  layout constant (constants/layouts.h), same test LoadMapFromWarp uses
  (overworld.c:856-859).
- `version` increments ONLY at the end of a complete READY build. Failed,
  synthetic, and invalid-identity builds leave DEGRADED with all records
  cleared and the version UNCHANGED — a caller comparing versions must
  always re-check status (GetBlock returns FALSE regardless of version), so
  a failed rebuild can never masquerade as fresh success. No partial
  worlds, no truncation.

## 6. Coordinate formulas (world block units, current map at (0,0))

| direction | neighbor origin | (audit §1.4; Fill*Connection-verified) |
|---|---|---|
| NORTH | `(offset, -cHeight)` | |
| SOUTH | `(offset, +currentHeight)` | |
| WEST  | `(-cWidth, offset)` | |
| EAST  | `(+currentWidth, offset)` | |

Local conversion: `localX = worldX - originX; localY = worldY - originY`
(block units). Current map: local == world.

## 7. Query behavior

`GetBlock(worldX, worldY)` applies the fail-closed gate FIRST: if
status != READY or currentBlocksSynthetic it returns FALSE immediately and
serves nothing — the caller's legacy RING/GRID/border path owns the frame
(on synthetic maps the runtime grid in sBackupMapData is the only real
content). Otherwise the precedence is:

1. **CURRENT** — `0 <= worldX < currentWidth && 0 <= worldY < currentHeight`.
2. **NEIGHBOR** — neighbors in DECLARATION order, first rect containing the
   point (gameplay's own selection order — GetMapConnectionAtPos/
   GetIncomingConnection iterate the same array first-match,
   fieldmap.c:693-698/758-790). Overlapping rects cannot disagree with
   gameplay by construction. (In the qualified data, no two spatial neighbor
   rects overlap at all — data-pinned in tests.)
3. **BORDER** — current map's border: index `i = ((x_bk+1)&1) +
   ((y_bk+1)&1)*2` with `x_bk = worldX + MAP_OFFSET`, `y_bk = worldY +
   MAP_OFFSET` (backup-frame parity; for x_bk/y_bk >= 0 this is byte-
   identical to GetBorderBlockAt at every GBA-visible coordinate); block =
   `currentLayout->border[i] | MAPGRID_IMPASSABLE`.

`ResolveBlockForRender` wraps GetBlock and adds: `metatileId =
UNPACK_METATILE(block)` (clamped to 0 if > NUM_METATILES_TOTAL, the
field_camera.c:243 rule), `layerType = UNPACK_LAYER_TYPE(ownerTileset
metatileAttributes[metatileId < NUM_METATILES_IN_PRIMARY ? metatileId :
metatileId - NUM_METATILES_IN_PRIMARY])` where ownerTileset =
ownerLayout->primaryTileset for ids < 512 else secondaryTileset — the exact
DrawMetatileAt resolution (field_camera.c:238-255), per OWNING map
(decision 9: owner tilesets; apron reconciliation is R11-I/P).

## 8. Connection edge cases (each covered by a named test)

- **Cycles** (A↔B): two records when B is current (back-connection);
  world-coordinate consistency is the 0-violation back-offset invariant
  (§11 test 19).
- **Same-direction duplicates** (Route111 2×WEST, Route124 2×EAST): two
  records, distinct origins; verified non-overlapping; precedence =
  declaration order.
- **Negative offsets** (18 in data; Route122→Route123 −100): formulas
  sign-agnostic; rects extend left/up of the active map's span.
- **Asymmetric dims** (Route111 40×140 ↔ Route113 100×20): per-record dims,
  no clamping; union bounds handle the extension.
- **Dive/emerge** (7+7 in data; Route124→Underwater_Route124): excluded —
  not spatial adjacency (gameplay treats them as warps: SetDiveWarp*,
  overworld.c:780-806).
- **One-way connections**: neighbor is present only while the source map is
  current (no back record required); rebuild on transition drops it.
- **Maps with no connections** (457 maps): neighborCount 0, union = current
  rect, BORDER beyond.
- **Synthetic grids** (battle pyramid floor / trainer hill):
  currentBlocksSynthetic → **status DEGRADED, neighborCount 0, no version
  bump**; GetBlock and ResolveBlockForRender return FALSE for EVERY world
  coordinate (inside and outside the rect — no BORDER, no placeholder
  blockdata). The caller's legacy RING/GRID/border path stays
  authoritative; the runtime-generated grid in sBackupMapData is the only
  real content. Direct test: §11 test 20.
- **Unpublished seam state** (pre-init / post-ClearMigratedEntries): layout
  .map/.border NULL → DEGRADED; GetBlock returns FALSE; renderer falls back
  to the legacy grid/border path (its existing behavior).

## 9. Object-event definition handling (decision 10)

Resident records store `const struct MapEvents *events` (canonical, view
only). `GetObjectEventsAt` returns the owning map's `objectEvents` +
`objectEventCount` (or the current map's for CURRENT cells; NULL for
BORDER). Future renderer/query stages resolve per-template graphics via the
R11-B seam `GetObjectEventGraphicsInfo(template->graphicsId)` — no new
resolution path is introduced. No neighbor NPC activation, no template
hydration, no events.inc/scripts.inc changes (they stay compiled —
data/map_events.s + data/event_scripts.s, both outside R11-E/F).

## 10. State v5 interaction (decision 11)

- **No format change, no serialized byte change.** The neighborhood never
  appears in a slice: its storage is host .data/.bss (no EWRAM_DATA/GBA_DATA
  attribute), outside BuildSlices' GAME_BSS/GAME_DATA (linker-script
  game-owned ranges) and EWRAM/IWRAM/COMMON (gba sections) by construction —
  the R6 trainer-table precedent (native_state.c restore comment).
- The two hook edits: (a) `NativeWorldNeighborhood_Init()` after
  `EmeraldResourceCompat_TryInitialize()` in desktop_game_content.c:755
  (inside the existing `#if NATIVE_LINUX` block); (b)
  `NativeWorldNeighborhood_Invalidate()` directly AFTER the
  Republish/ClearMigratedEntries if/else in native_state.c:2910 (inside the
  existing `#if PLATFORM_SDL2 && NATIVE_LINUX` block). Neither touches
  slices, BuildSlices, or the file format; the existing state test suite
  must stay byte-green.
- Proofs (§11 tests 16): section attribution (nm/readelf — symbol in host
  .bss/.data, not gba_*), range non-intersection (harness asserts the
  struct address outside __start/__stop_gba_ewram/iwram/common and outside
  Platform_RuntimeGetGame{Data,Bss}Range), behavioral (corrupt the records,
  save + load in the cross-restart harness → rebuild from identity, garbage
  gone, fresh pointers).

## 11. Tests

**Harness A — hand-built fixtures** (`tests/emerald_native_world_neighborhood_test.c`
+ `tests/run_emerald_native_world_neighborhood.sh`): compiles the module +
host_memory.c + a stub `Overworld_GetMapHeaderByGroupAndId` (the verbatim
PORTABLE-branch body, overworld.c:595-603, over a fixture gMapGroups GbaAddr
table) + hand-built MapHeader/MapLayout/MapEvents/connection records + a
`gSaveBlock1Ptr` fixture. Covers the pure math with fully controlled inputs:
2 (N/S math), 3 (E/W math), 4 (positive offsets), 5 (negative offsets),
6 (asymmetric dims), 7 (multiple neighbors), 8 (duplicates + cycles),
12 (outside-union → BORDER parity incl. IMPASSABLE bit), 13 (current
precedence with an adversarially overlapping fixture).

**Harness B — real qualified data** (`tests/emerald_native_world_real_test.c`
+ `tests/run_emerald_native_world_real.sh`): the production-runner link set
(gen3 core + all compat seams + umbrella + session, as
run_emerald_trainer_native_compat_production.sh) PLUS
`build/linux64/data/maps.o`, `build/linux64/data/map_events.o`,
`build/linux64/data/event_scripts.o`, host_memory.c, the tileset-anim stubs,
the Overworld stub, and a real `struct SaveBlock1` fixture. Runs against the
production pack (real published blockdata/tilesets). Covers:
1 (Littleroot↔Route101 both directions: origins (0,−20)/(0,+20), Oldale),
9 (no-connection interior: LittlerootTown_MaysHouse_1F — neighborCount 0),
10 (query inside current), 11 (query inside neighbor), 14 (identity mutation
→ version bump + rebuild, warps and transitions), 17 (no second-hop:
Route101 current → residents {Route101, Oldale, Littleroot} only; Oldale's
Route103/Route102 not resident), 18 (Route124 → 4 spatial neighbors, no
Underwater_Route124), 19 (data-wide pin sweep: exact counts 148 total =
134 spatial + 7 dive + 7 emerge; 61 maps with ≥1 spatial connection and 3
dive/emerge-only; max 4 spatial neighbors per map; max 2 same-direction;
0 back-connection violations across the 134 spatial records; 18 negative
offsets; the two duplicate pairs' rects non-overlapping), 20
(synthetic-map fail-closed: real pyramid map identity → status DEGRADED +
currentBlocksSynthetic TRUE, neighborCount 0, version UNCHANGED, GetBlock
FALSE inside the map rect AND outside it, ResolveBlockForRender FALSE —
the legacy RING/GRID path owns every query).

**Harness C — state proofs**: extend `tests/emerald_resource_state_test.c`
(+ runner) with the neighborhood module + maps.o + host_memory + stub: 15
(rebuild after state restore — corrupted records, save, load, fresh
rebuild), 16 (not-serialized, three layers of §10).

**Harness D — R11-F renderer-facing proof** (`tests/emerald_native_world_
render_proof.c` + runner, checkpoint 7): real pack + real data (Harness B
link set). Offscreen tilemap-entry render (NO pixels, NO SDL, NO viewport
code): rect world blocks x ∈ [0,20), y ∈ [−10,10) — 480×160 px at 8 px/tile,
60×20 tiles, WIDER than the 240×160 view and crossing the real
Littleroot→Route101 north connection (world y = 0): for every block, call
`NativeWorldNeighborhood_ResolveBlockForRender` and write the 4 entries per
BG into three 60×20 tilemap buffers via
`NativeWorldNeighborhood_MetatileTileEntryForLayer`; assert (i) the rect
exceeds 240×160, (ii) rows y < 0 come from Route101's blockdata with
Route101's tilesets and rows y ≥ 0 from Littleroot's, (iii) the buffers
byte-match an INDEPENDENT oracle that reimplements the resolution by hand
directly from the two layouts' published .map/.border + metatile tables
(the two-independent-derivations pattern), (iv) the source enum flips
exactly at y = 0 and stays BORDER nowhere inside the rect, (v) the
presentation path is untouched (checkpoint-gate diffs).

**Existing battery**: all 18 runners + sanitizers must stay green
(isolation 19953/4518 in particular — the module adds zero payload bytes).

## 12. Implementation checkpoints (flash, in order; stop on any failure)

1. Header + module skeleton (structs, Init/Shutdown/Invalidate/EnsureCurrent
   shell, the house guard). Native release build compiles and links
   (`make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 -B rom`); binary contains
   the module symbols; GBA-path untouched (git diff on non-platform files
   empty except docs/tests).
2. Builder + coordinate formulas + precedence + border parity +
   ResolveBlockForRender + MetatileTileEntryForLayer + object-event
   accessor. `git diff --check` clean.
3. Harness A green (synthetic geometry, all math/edge tests).
4. Harness B green (real data + production pack; all map tests + data-wide
   pins).
5. Lifecycle hook edits (desktop_game_content.c Init; native_state.c
   Invalidate). Full native rebuild (release + DINFO) — both boot; state
   test suite green (format untouched).
6. Harness C green (cross-restart rebuild + not-serialized proofs).
   **R11-E complete.**
7. Harness D green (480×160 offscreen proof across the real connection;
   presentation-path diffs empty; zoom off). **R11-F complete.**
8. Full battery: all 18 runners + sanitizers green; isolation unchanged
   (19953 ok / 0 failed, 4518/4518).
9. `git diff --check`; release (0 debug) + DINFO (7 debug) verified via
   readelf -S; distinct Build IDs; write
   `docs/R11EF_NATIVE_WORLD_NEIGHBORHOOD_REPORT.md` with the manual
   validation checklist (§14); **STOP for manual validation.**

## 13. Stop conditions

- Any checkpoint fails → fix with source-tree evidence; no redesign without
  evidence.
- GBA parity broken → stop: the ONLY production-file edits allowed are the
  two guarded hook insertions in src/platform files (not compiled by the
  GBA build). `git diff --name-only` outside src/platform/tests/docs must
  be empty at every checkpoint.
- Presentation path touched → stop: desktop_video.c,
  native_overworld_renderer.c, native_overworld_viewport.c,
  native_field_compositor.c, field_camera.c must have ZERO diff in E/F.
- Second resource system / second pack path / pack format change / State v5
  format change / GBA behavior change / commit / push: all prohibited.
- Battery red / isolation count change: stop.

## 14. Manual validation checklist (E/F changes nothing visible — smoke gate)

DINFO build first; then the release binary:

1. Boot to Littleroot Town — map renders normally at 240×160
2. Walk Littleroot → Route 101 north transition — connection animation
   identical to pre-R11 (strip visuals, camera behavior)
3. Walk back Route 101 → Littleroot (south transition) — identical
4. Warp into an interior (player's house) and back out
5. Save state in the overworld → quit → relaunch → load → map identical,
   transitions still work
6. Repeat once
7. Pixel-identical to pre-R11 behavior on the whole path
8. No zoom activated anywhere (viewport stays 240×160; no viewport env
   override used)
9. Route 111 area if reachable (west double-connection): Route 113 and
   Route 112 connections both behave
10. DINFO build boots to the overworld without errors
