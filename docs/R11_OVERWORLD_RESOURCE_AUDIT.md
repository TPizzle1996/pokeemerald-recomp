# R11-A — Overworld Resource Graph Audit

Scope: establish the exact current overworld rendering/resource path before any
R11 migration code. No code was modified during this audit.

## 1. Structure inventory (definitions and locations)

### 1.1 Object-event graphics

- `struct ObjectEventGraphicsInfo` — include/global.fieldmap.h:297-315. Fields:
  `tileTag, paletteTag, reflectionPaletteTag, size, width, height,
  paletteSlot/shadowSize/inanimate/disableReflectionPaletteLoad (bitfield),
  tracks, oam (const struct OamData*), subspriteTables (const struct
  SubspriteTable*), anims (const union AnimCmd* const*), images (const struct
  SpriteFrameImage*), affineAnims (const union AffineAnimCmd* const*)`. The
  five pointer members are native-width on LINUX64 (host pointers into
  compiled .rodata — image range, persistent-address encodable).
- Central accessor: `GetObjectEventGraphicsInfo(u8 graphicsId)` —
  src/event_object_movement.c:1917-1930. Resolves through
  `gObjectEventGraphicsInfoPointers[]` (src/data/object_events/
  object_event_graphics_info_pointers.h), with three special paths:
  `OBJ_EVENT_GFX_VARS` dynamic ids (var-driven), `OBJ_EVENT_GFX_BARD`
  (Mauville old man variant table), and an out-of-range clamp to
  `OBJ_EVENT_GFX_NINJA_BOY`. This is the ONE centralized seam the migration
  must keep authoritative.
- Data files (src/data/object_events/):
  - `object_event_graphics_info.h` — the const info structs (designated
    initializers pointing at compiled oam/subsprite/anims/images/affine
    tables).
  - `object_event_graphics.h` — the LEAF payloads: `gObjectEventPic_*` =
    `INCBIN_U32` RAW 4bpp sheets (e.g. `walking.4bpp` — NOT LZ-compressed),
    `gObjectEventPal_*` = `INCBIN_U16` gbapal (16×u16), plus the
    `sPicTable_*` SpriteFrameImage arrays whose `.data` fields point at the
    raw sheets.
  - `object_event_anims.h` — compiled `union AnimCmd` tables
    (sAnimTable_*).
  - `base_oam.h`, `movement_action_func_tables.h`,
    `movement_type_func_tables.h` — compiled OamData/subsprite/movement
    tables (functional, must remain compiled).
  - `object_event_graphics_info_pointers.h` — the id→info pointer table.
  - Berry tree graphics tables (berry_tree_graphics_tables.h) — variant
    object graphics (berry stages) resolved through
    `gBerryTreeObjectEventGraphicsIdTablePointers` /
    `gBerryTreePicTablePointers` / `gBerryTreePaletteSlotTablePointers`
    (src/event_object_movement.c:1904-1912).
- Consumers of `.images`/`.anims`/`.oam`: src/event_object_movement.c
  (object template hydration at 1403-1610, sprite setup at 1749-1917),
  src/field_camera.c (CurrentMapDrawMetatileAt region), the sprite
  pipeline (`struct Sprite` images/anims fields — already modeled by the
  R10 walker's known-location set for gSprites).

### 1.2 Tilesets

- `struct Tileset` — include/global.fieldmap.h:55-64: `isCompressed,
  isSecondary, tiles (const u32*), palettes (const u16 (*)[16]),
  metatiles (const u16*), metatileAttributes (const u16*), callback
  (TilesetCB = void(*)(void))`. Pointers native-width on LINUX64; callback
  is compiled code.
- Data files (src/data/tilesets/):
  - `headers.h` — const struct Tileset gTileset_* designated initializers
    (primary = General etc.; secondaries = Petalburg, Rustboro, ...) with
    `.callback = InitTilesetAnim_<X>` (compiled functions in
    src/tileset_anims.c).
  - `graphics.h` — LEAVES: `gTilesetTiles_*` = `INCBIN_U32` LZ-compressed
    4bpp (`tiles.4bpp.lz`); `gTilesetPalettes_*[][16]` = 13×
    `INCBIN_U16` gbapal files per tileset.
  - `metatiles.h` — LEAVES: `gMetatiles_*` = `INCBIN_U16`
    `metatiles.bin` (u16 words; 512 primary + 512 secondary, each metatile =
    8 tiles), `gMetatileAttributes_*` = `INCBIN_U16`
    `metatile_attributes.bin` (behavior = bits 0-7, layer type = bits
    12-15; masks in global.fieldmap.h:30-38).
- Loading/animation: `CopyMapTilesetsToVram` / `CopyPrimaryTilesetToVram` /
  `CopySecondaryTilesetToVram(UsingHeap)` / `LoadMapTilesetPalettes` /
  `LoadSecondaryTilesetPalette` (field_camera.c + overworld.c:539 region),
  animated-tile update path in src/tileset_anims.c (compiled callbacks
  apply to the live tileset VRAM/tilemaps; the tileset anim machinery is
  functional code and stays compiled).
- Constants: NUM_TILES_IN_PRIMARY 512, NUM_TILES_TOTAL 1024,
  NUM_METATILES_IN_PRIMARY 512, NUM_METATILES_TOTAL 1024,
  NUM_PALS_IN_PRIMARY 6, NUM_PALS_TOTAL 13 (include/fieldmap.h:4-10).

### 1.3 Maps

- `struct MapLayout` — global.fieldmap.h:66-74: `s32 width, height; const
  u16 *border; const u16 *map; const struct Tileset *primaryTileset;
  const struct Tileset *secondaryTileset` (native-width pointers on
  LINUX64).
- `struct MapHeader` — global.fieldmap.h:185-205: native-width pointers
  (mapLayout/events/mapScripts/connections), STATIC_ASSERT sizeof == 0x30
  on LINUX64 (MapHeaderNativeSize), with per-field offsets asserted
  (0x00/0x08/0x10/0x18) — the native map header layout is already a
  widened host structure.
- `struct MapEvents` — 0x28 on native (counts u8×4 + 4 native pointers),
  asserted.
- `struct MapConnection` — global.fieldmap.h:171-177: `u8 direction; s32
  offset; u8 mapGroup; u8 mapNum`, STATIC_ASSERT 0x0C — GBA-shaped,
  byte-stable, contains NO addresses (map identity only).
- `struct MapConnections` — `s32 count; const struct MapConnection
  *connections`.
- Map data source: `data/maps/<MapName>/{map.json, header.inc, events.inc,
  connections.inc, scripts.inc}` + `data/layouts/<Layout>/{layout.json,
  map.bin, border.bin}`. map.json carries id/layout/music/weather/flags +
  the connection list `{map, offset, direction}` + object/warp/coord/bg
  event arrays. Assembled via data/maps.s → data/layouts/layouts.inc,
  layouts_table.inc, data/maps/{headers,groups,connections}.inc. The
  generated `header.inc` uses `host_ptr` directives — the native build
  emits host-width map header records in a GbaAddr-resolved table.
- Native table resolution: `Overworld_GetMapHeaderByGroupAndId`
  (src/overworld.c:595-603, PORTABLE branch) — `gMapGroups[mapGroup]` is a
  `GbaAddr`; the group table is resolved with `HostResolveGbaAddr` and
  entries with `HostResolveGbaTableEntry`. So the native map header TABLES
  are GBA-shaped 4-byte address tables (GbaAddr words) resolved at
  runtime — the "GBA-shaped 4-byte address" class.
- Map grid: `sBackupMapData[MAX_MAP_DATA_SIZE]` (EWRAM,
  src/fieldmap.c:28) + `gBackupMapLayout` (COMMON_DATA) — the MAP_OFFSET
  (7)-expanded current-map buffer: u16 blocks = metatile id (10 bits) |
  collision (2) | elevation (4) (global.fieldmap.h:5-25). Accessors:
  `MapGridGetMetatileIdAt` (fieldmap.c:365), `GetMapGridBlockAt` (border
  fallback via `GetBorderBlockAt`), `GetCameraCoords` (fieldmap.c:810).
- Current-map state: `EWRAM_DATA struct MapHeader gMapHeader`
  (fieldmap.c:29); `LoadCurrentMapData` (overworld.c:610-617) copies the
  compiled header by value and patches `.mapLayout = GetMapLayout()`.
- Live current map header accessors: `gSaveBlock1Ptr->location.{mapGroup,
  mapNum}`, warp machinery (overworld.c:619-680).

### 1.4 Connections (the composition algorithm)

`InitBackupMapLayoutConnections` (src/fieldmap.c:133-172) iterates the
ACTIVE map's `connections`; per direction the neighbor's strip is copied
into the MAP_OFFSET-expanded backup grid (`FillConnection`,
fieldmap.c:174-193):

- SOUTH: backup dest y = height + MAP_OFFSET, src y2 = 0; x = offset +
  MAP_OFFSET with the negative-offset clamp (x2 = -x when x < 0).
- NORTH: backup dest y = 0, src y2 = cHeight - MAP_OFFSET (the neighbor's
  LAST MAP_OFFSET rows); same x/offset logic.
- WEST: backup dest x = 0, src x2 = cWidth - MAP_OFFSET; y = offset +
  MAP_OFFSET with the same clamp on y.
- EAST: backup dest x = width + MAP_OFFSET, src x2 = 0.

World-coordinate consequence (the R11-F algorithm): with the active map at
origin (0,0), a connection with offset o places the neighbor's origin at

- NORTH: (o, -cHeight)
- SOUTH: (o, +height)
- WEST:  (-cWidth, o)
- EAST:  (+width, o)

where the neighbor's block column/row x2/y2 = active coordinate - o aligns
the strips (verified against Fill*Connection's src/dest mapping). The
MAP_OFFSET expansion exists only for the GBA backup grid; the neighborhood
world space needs no expansion — it holds full maps at their computed
origins.

Runtime queries: `GetMapConnectionAtPos` (fieldmap.c:758) uses
`IsPosInConnectingMap` (x - offset in [0, cWidth) for N/S; y - offset in
[0, cHeight) for E/W); `IsPosInIncomingConnectingMap` handles the incoming
side (which neighbor a standing position belongs to — the map-transition
semantics).

### 1.5 Rendering / camera

- Camera state: `static struct FieldCameraOffset sFieldCameraOffset`
  (field_camera.c:36) — x/yPixelOffset + x/yTileOffset into the 32×32 BG
  ring; `sHorizontalCameraPan`/`sVerticalCameraPan`; `struct Camera
  gCamera` (global.fieldmap.h:404-409) — active/x/y.
- `FieldUpdateBgTilemapScroll` (field_camera.c:74-84) writes BG1/2/3
  HOFS/VOFS from the camera offsets (+8 Y bias).
- `DrawWholeMapView` / `DrawWholeMapViewInternal` (106-133): renders the
  visible 15×10 metatiles around `gSaveBlock1Ptr->pos` into the 32×32
  ring at `(xTileOffset, yTileOffset)`; `DrawMetatileAt` →
  `DrawMetatile` paints 4 tiles per metatile into the BG1/2/3 tilemaps.
  Camera pans redraw only the new slice (RedrawMapSlice*).
- The visible rectangle is derived from the GBA screen: the current
  240×160 view = 30×20 tiles = 15×10 metatiles around the camera focus.
- Pre-existing native renderer workstream (uncommitted, NOT part of R11):
  src/platform/native_overworld_renderer.c + snapshot/viewport/parity
  modules render the BG1/2/3 composite from
  `NativeOverworld_CaptureSnapshot()`; `NativeViewport` (240-360 px,
  scale 1.0-1.5 experimental zoom). The R11 camera seam must be a clean
  WORLD-coordinate seam independent of this experimental zoom (which the
  R11 directive forbids shipping).

## 2. Four-way classification

1. **Leaf payloads** (migration candidates — ROM_BASE):
   - object-event raw 4bpp sheets (`gObjectEventPic_*`),
   - object-event gbapal palettes (`gObjectEventPal_*`, incl. reflection
     palettes),
   - tileset LZ 4bpp tiles (`gTilesetTiles_*`),
   - tileset gbapal palettes (`gTilesetPalettes_*[13]`),
   - tileset metatile words (`gMetatiles_*`) and metatile attributes
     (`gMetatileAttributes_*`) — in scope "where appropriate/resource-
     owned" (R11-C),
   - map blockdata + border words (`data/layouts/*/map.bin`,
     `border.bin`) — in scope per R11-D.

2. **Host-native cloneable pointer graphs** (hydrate into session storage):
   - `struct ObjectEventGraphicsInfo` (+ the id→info pointer array) with
     images/anims/oam/subsprite/affine pointers retained as compiled
     references; the IMAGE arrays (`sPicTable_*` SpriteFrameImage[]) are
     cloneable and their `.data` must point at session payloads,
   - `struct Tileset` objects (cloneable; `.callback` stays compiled),
   - `struct MapLayout` views (blockdata/border/tileset pointers),
   - `struct MapHeader`/`MapEvents` views (map-local logic identity stays
     compiled; the neighborhood holds VIEWS, never copies of events/
     scripts).

3. **GBA-shaped structures containing 4-byte addresses** (must never be
   widened in place):
   - `gMapLayouts[]` / `gMapGroups[]` — 4-byte `GbaAddr` index tables
     (data/layouts/layouts_table.inc, data/maps/groups.inc) resolved via
     `HostResolveGbaAddr`/`HostResolveGbaTableEntry`
     (src/overworld.c:96-102, 544-554, 595-603). The RECORDS they index
     (MapLayout, MapHeader) are emitted with `host_ptr` (host-width) on
     the native build — see data/layouts/layouts.inc and
     data/maps/*/header.inc.
   - `struct ObjectEventTemplate.script` — GbaAddr with
     `ObjectEventTemplate_{Get,Set}Script` inline accessors
     (global.fieldmap.h:99-124),
   - `struct MapConnection` (0x0C, no addresses — pure identity +
     offset), `WarpEvent` (0x08), `ObjectEvent` (0x24) — GBA-shaped
     records with STATIC_ASSERTs.
   - Note: `struct Tileset` / `struct MapLayout` / `struct MapHeader` /
     `struct ObjectEventGraphicsInfo` have NO native branches — compiled
     C per-target, so their pointer members are native-width on LINUX64
     (MapHeader/MapEvents sizes asserted 0x30/0x28). The non-PIE link
     keeping vanilla generated data below 4 GiB (host_memory.c:325-329)
     is what makes the remaining 4-byte address VALUES valid on native.
   - Invariant: GBA logical = 32-bit GbaAddr; host = native width; never a
     native pointer in a 4-byte GBA field; never widen a GBA-shaped
     record in place — hydrate native-width clones instead.

4. **Compiled functional code** (stays compiled): tileset animation
   callbacks (`InitTilesetAnim_*` + the tileset_anims.c machinery), object
   event anim cmd tables (`union AnimCmd` — data, but functionally
   consumed as animation commands), OamData/subsprite tables, movement
   type/action tables, map scripts, event scripts, warp/coord/bg handling,
   collision, object-event simulation.

## 3. R10 State v5 interaction

New persistent resource pointers (hydrated info/tileset/map views whose
fields enter serialized game memory) must register with the reverse
resource-range index: the compat images' stream regions for object-event
sheets/palettes, tileset tiles/palettes, and blockdata payloads — the same
R10 mechanism (identity + role + offset; zeroed in-band; transactional
resolution). The neighborhood cache itself is derivable (rebuild
post-load), never serialized.

## 3b. Map-family inventory (parallel sweep results)

| item | count |
|---|---|
| maps (data/maps/*/map.json) | 518 |
| layout dirs (border.bin + map.bin) | 441 |
| map groups (gMapGroups entries) | 34 |
| maps with ≥1 connection | 64 |
| connection records (total) | 148 |
| max connections on one map | 5 (Route124) |
| blockdata width range / mode | 1–140 / 15 (30 maps) |
| blockdata height range / mode | 1–140 / 8 (56 maps) |
| largest blockdata | 6400 blocks (Underwater_Route127_Layout, 80×80) |
| smallest blockdata | 1 block (BattleFrontier_BattlePikeRoomUnused_Layout) |
| border.bin | 4 u16 (2×2), uniform across all 441 layouts |

- No `gMapHeaders`/`gMapConnections` globals: per-map labels are collected
  into `gMapGroup_*` tables in `data/maps/groups.inc` with a 34-entry
  `gMapGroups` dispatch (GbaAddr words); `host_ptr`/`gba_ptr` macros
  (asm/macros/map.inc:4-17) emit host-width vs 4-byte GBA pointers.
- Generation: `tools/mapjson/mapjson` + `map_data_rules.mk` produce
  header/events/connections .inc + include/constants/{map_groups,layouts,
  map_event_ids}.h from map.json/layouts.json; the map.bin/border.bin
  files are checked-in extracted assets with no generator.
- scripts.inc is hand-authored assembly (468 files, some shared), NOT
  generated — outside R11 scope (R13).
- **Gen3 resource coverage today: ZERO overworld entries.** The catalog
  (resources/catalogs/emerald/catalog.toml) holds exactly the 196 trainer
  battle resources; extraction manifests are trainer-only.
  `GEN3_RESOURCE_TYPE_TILESET`/`TILEMAP` already exist in the runtime type
  vocabulary (resource_types.h:15-16, wired in emerald_resource_import.c),
  so no type-system change is needed for R11 families.

## 4. Audit conclusions

- The centralized seams exist: `GetObjectEventGraphicsInfo` (object
  events) and the `struct Tileset`/`MapLayout`/`MapHeader` structures
  (tilesets/maps) with the native build ALREADY using widened host
  pointers for MapHeader/MapEvents (asserted layouts). The migration adds
  session-owned payload arenas + cloned info/tileset graphs behind those
  same seams.
- The connection composition math is fully specified by
  Fill{N,S,E,W}Connection and yields the world-origin formulas in §1.4;
  the neighborhood can reproduce it exactly without touching gameplay.
- The pre-existing native overworld renderer/snapshot workstream
  (uncommitted) already models LOGICAL world origin
  (cameraMapX*16 + cameraX) — the R11 camera seam should align with that
  convention.
### Object-event / tileset inventory (second parallel sweep)

Object-event family:
- 245 `ObjectEventGraphicsInfo` structs; `gObjectEventGraphicsInfoPointers[]`
  = 239 entries (NUM_OBJ_EVENT_GFX) + 7 Mauville old-man variants;
  OBJ_EVENT_GFX_VARS = 240.
- Leaves: 253 `gObjectEventPic_*` RAW 4bpp sheets, 35 `gObjectEventPal_*`
  gbapal (29 null placeholders); co-located field-effect leaves (37 pics,
  6 pals) sit in the same header.
- Pointer graphs: 249 `sPicTable_*` SpriteFrameImage arrays (1,690
  `overworld_frame` entries — pointer+offset views INTO the sheets), 15
  anim tables / 101 anim sequences (compiled `union AnimCmd` data), 1
  affine table / 6 sequences, 8 subsprite tables, 8 base OamData. No
  function-pointer members — everything data.
- Variants: berry trees (30 per-berry pic tables + master tables, driven
  by item id at event_object_movement.c:1905-1914), Mauville old men.

Tileset family:
- 75 `struct Tileset` (3 primary: General/Building/SecretBase; 8
  uncompressed; 50 with NULL callback).
- Leaves: 83 `gTilesetTiles_*` (LZ 4bpp; 8 unreferenced parity leaves),
  75 `gTilesetPalettes_*[][16]` (1,200 gbapal files), 70
  `gMetatiles_*` + 70 `gMetatileAttributes_*` u16 bins, 135 tileset
  anim-frame 4bpp leaves.
- Compiled: 25 `InitTilesetAnim_*` + 41 static `TilesetAnim_*` frame
  callbacks (src/tileset_anims.c), 31 `gTilesetAnims_*` frame tables.
- No `struct MetatileAttributes` exists — attributes are raw u16 words
  (behavior bits 0-7, layer bits 12-15), split primary/secondary on
  NUM_METATILES_IN_PRIMARY (fieldmap.c:409-424).

Resource-system ties: none — catalog = 196 trainer battle resources only;
`GEN3_RESOURCE_TYPE_TILESET`/`TILEMAP` exist in the type vocabulary
already, so R11 families need no type-system change.
