# Native overworld renderer architecture

Research date: 2026-08-13

Status: architecture recommendation; no renderer rewrite is implemented by this
document.

## Executive decision

A PC-native overworld renderer is feasible, and it is a better long-term design
than extending the current "GBA center plus reconstructed margins" compositor.
The renderer should be a read-only presentation backend fed by an immutable
snapshot of Emerald's live state. It should render a complete frame at every
supported logical viewport, including 240x160. Emerald must continue to own all
simulation, map loading, object movement, scripts, collision, saves, and battle
transitions.

The recommended data path is:

```text
Emerald update / animation / map-loading code
                 |
                 v
       NativeOverworldSnapshot
       - fixed-point camera and map identity
       - mutable map grid and map-connection view
       - final BG tilemap overrides
       - sprite draw-command stream
       - current BG/OBJ VRAM and palette state
       - weather/effect/capability state
                 |
                 v
       NativeOverworldRenderer
       - map layers and exact priority compositor
       - generic OBJ/sprite compositor
       - supported weather and field effects
                 |
                 v
       arbitrary logical viewport -> SDL presentation scaling
```

This is not a second game engine. The snapshot adapter should reuse Emerald's
decisions after they have been made, especially animation frame selection,
palette allocation, and sprite positioning. The first renderer should be a
simple CPU BGR555 reference implementation. A cached or GPU implementation can
replace it only after pixel-parity tests exist.

There is one important qualification: terrain expansion is straightforward,
but correct NPC expansion is not. Vanilla Emerald has only 16 object-event
slots and does not instantiate events belonging to a connected map before a map
transition. A wider renderer can therefore show valid terrain for which no live
NPC state exists. Object streaming is a separate, gameplay-sensitive project
and must not be hidden inside renderer code.

## 1. Best overall architecture

Introduce four narrow layers rather than one generalized compositor.

### 1.1 Snapshot/capability adapter

Build one immutable `NativeOverworldSnapshot` at a defined point after sprite
animation/OAM construction and after the frame's VRAM/palette transfers. The
snapshot should contain values or bounded copies, not long-lived raw assumptions
about mutable globals. Copying the GBA-sized VRAM and palette regions is cheap
enough for a reference renderer and avoids timing-dependent mixed frames.

The adapter should report capabilities required by the current scene, for
example `MAP`, `SPRITES`, `AFFINE_OBJ`, `OBJ_BLEND`, `BG0`, `WINDOWS`,
`SCANLINE_EFFECT`, and `WEATHER_EXTENSION`. The native backend is selected only
when it supports every required capability. This replaces a single brittle
"ordinary overworld" predicate with an explainable fallback decision.

### 1.2 World tile provider

Expose a query in world 8x8-tile coordinates that returns the three overworld BG
tile entries. Its sources, in precedence order, should be:

1. The final live 32x32 BG ring when a coordinate is represented there. This
   preserves transient writes such as animated doors that do not necessarily
   round-trip through `MapGridGetMetatileIdAt`.
2. The mutable backup map grid, metatile attributes, and primary/secondary
   metatile tables.
3. A connection-aware resolver for coordinates outside the backup grid.
4. The repeating 2x2 map border when there is no applicable connection.

The renderer should sample the current BG character memory and palette, not
source PNGs. Tileset animation already materializes its selected frames into BG
VRAM, and weather/fades already materialize adjusted colors into palette RAM.

### 1.3 Generic sprite command stream

Factor the semantic part of Emerald's sprite-to-OAM path so it can emit native
draw commands before hardware coordinate wrapping and normal-screen culling.
A command needs a signed anchor, shape/size or subsprite geometry, current tile
base, palette slot, flips/affine matrix, OBJ mode, mosaic, priority,
subpriority/OAM order, and its relationship to camera coordinates.

Use `gSprites` and the associated object event to retain signed position and
world ownership, but sample current OBJ VRAM and OBJ palette RAM for pixels.
That combination reuses Emerald's animation, decompression, dynamic palette,
and field-effect machinery without making 8-bit/9-bit OAM coordinates the
source of world truth.

### 1.4 Complete native compositor

Render BG layers and all supported OBJ commands into a complete target, even at
240x160. Reuse or factor the priority, window, affine, and blend algorithms from
this repository's GBA drawing code where practical. Do not paste the original
240x160 result into the middle. SDL should upload a dynamically sized logical
texture and then scale that texture to the PC window; a 4K window should not
imply rendering a 4K tile world unless explicitly requested.

## 2. Why the current center-plus-margins design is limiting

The current implementation is useful evidence but is structurally split into
two renderers. [`native_overworld_renderer.c`](../src/platform/native_overworld_renderer.c)
reconstructs map pixels from metatiles and draws a subset of sprites, then lines
457-461 overwrite the entire 240x160 center with `DrawFrame` output. That creates
a seam wherever the two paths disagree on timing, priority, blend, transient BG
writes, sprite visibility, or effects.

Specific limitations are:

- Center and margins have different truth sources. A discrepancy is concealed
  until it crosses the 240x160 boundary.
- The margin sprite path skips affine, non-normal OBJ modes, mosaic, and some
  priority behavior, and recognizes shadows/surf through callback identities.
- It iterates object events rather than all field sprites, so unrecognized field
  effects and weather sprites disappear outside the center.
- It consumes only currently instantiated objects; it cannot solve object
  streaming by rendering harder.
- Fixed buffers and enum values assume 300x200 or 360x240 rather than a viewport
  contract.
- `Overworld_IsNativeExpandedRendererReady` in
  [`overworld.c`](../src/overworld.c) checks many exact callbacks/register values.
  It is safe as an experimental gate, but every legitimate overworld variation
  becomes another exception.
- The desktop presentation path first renders the GBA frame and then conditionally
  renders the expanded texture. Its non-integer destination policy remains 3:2,
  which is incompatible with a true arbitrary-aspect world view.

A full native 240x160 path makes discrepancies observable and testable instead
of hiding them under the authoritative center.

### Current live-state inventory

| State/system | Already available in this port | Current native-margin use | Recommended interface |
|---|---|---|---|
| Map coordinates | `gSaveBlock1Ptr->pos`, object current/initial coordinates | Yes | Snapshot signed world origin and per-object ownership |
| Current layout | `gMapHeader.mapLayout`, primary/secondary tilesets | Yes | Read-only map provider |
| Mutable map | `gBackupMapLayout`, `MapGridGetMetatileIdAt`, collision/elevation/behavior accessors | Metatile ID/layer only | Snapshot/query mutable blocks; retain collision/elevation as object metadata, not renderer logic |
| Metatile layers | `DrawMetatile` and attributes define split/covered/normal mapping | Yes | Factor/shared pure tile-entry resolver |
| Final BG tilemaps | `gOverworldTilemapBuffer_Bg1/Bg2/Bg3`, 32x32 ring | Readiness check only | Highest-precedence override inside the represented ring |
| BG graphics | Current `BG_CHAR_ADDR(0)` | Yes | Bounded frame snapshot; carries tileset animation results |
| BG palettes | Current `PLTT` / palette transfer state | Yes | Capture the final frame palette at a defined point |
| Object events | 16-entry `gObjectEvents`, templates in `SaveBlock1` | Player/NPC plus selected links | Ownership, world anchor, elevation, intentional visibility; not the pixel source |
| Sprites/frames | 64-entry `gSprites`, anim state, subsprites, OAM fields | A restricted non-affine subset | Emit generic pre-wrap native draw commands |
| OBJ graphics/palettes | Current OBJ VRAM and OBJ PLTT | Yes | Bounded frame snapshot; pixel/color source |
| Camera and pan | field camera tile/pixel offsets and horizontal/vertical pan | Yes | Signed fixed-point `CameraSnapshot` |
| Connections/borders | Backup-grid copied strips, connection records, border helpers | Implicitly through map grid | Backup grid first, direct connection resolver later |
| Weather | Weather state, palette transforms, blend registers, weather sprites | Only final palette/recognized objects indirectly | Final palette + generic sprites + explicit wider-pattern providers |
| Reflections/surf/shadows | Linked field-effect sprites and object-event fields | Only shadow and player surf callbacks | Generic sprite stream and exact compositor |
| Other field effects | Mostly sprite/task-driven; some BG0/window/scanline effects | Generally absent in margins | Generic sprites; capability-gated special providers/fallback |
| Tile animation | Scheduled transfers update BG VRAM before display | Inherited by sampling VRAM | Preserve by snapshotting materialized BG VRAM |

This inventory is why a bolt-on renderer does not need to decode source assets
or recreate animation state. Most necessary presentation data already exists;
it needs a stable, viewport-independent interface and exact composition.

## 3. Porymap findings

[Porymap](https://github.com/huderlem/porymap) is the closest mature reference
for arbitrary map-region rendering. The reviewed revision was
`26b919d` (2026-04-12).

Its useful patterns are:

- `Layout::render` renders an arbitrary/full layout into a `QImage`, caches
  rendered metatile images by metatile ID for a pass, and iterates block data.
- Its image provider selects primary or secondary tileset data, resolves each
  metatile's layer type, applies per-tile palette and horizontal/vertical flip,
  and makes palette index zero transparent.
- Its border renderer repeats the border image outside a layout.
- Connection export clips/stitches cardinal connected maps. Connected map tiles
  are interpreted using the parent/current layout's tileset context, matching
  the assumptions made by Emerald's connection loader.
- Its map-image exporter can traverse a connection graph and compose borders,
  maps, then event markers over a region.

What it does not provide is equally important. Porymap parses project assets and
uses Qt painting; it is not driven by live `MapGridSetMetatileIdAt` mutations,
VRAM animation transfers, palette fades/weather, sprites, camera transitions,
or GBA composition rules. No runtime animated-tiles solution was found in the
reviewed map renderer.

Recommendation: adopt the data-flow ideas—region queries, metatile caching,
primary/secondary resolution, border repetition, and connection transforms—but
independently implement them against Emerald's live data. Emerald's own
`DrawMetatile` layer mapping is more authoritative for this runtime than
Porymap's Qt renderer.

## 4. pokeemerald-expansion findings

The current [pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion)
revision reviewed was `4189835` (2026-08-13). It contains valuable advanced
overworld behavior, including configuration and implementations for large
overworld graphics, followers, dynamic follower palettes, shadows, reflections,
light sprites, and large/asymmetric/subsprite cases.

These systems reinforce the generic sprite-command approach:

- Large 48x48/64x64 and asymmetric graphics cannot be safely reduced to a
  hard-coded NPC rectangle.
- Followers and reflections create linked sprites and update palette/elevation
  behavior; the native backend should consume the resulting live sprite state.
- Dynamic palettes are already assigned and processed by game code, so current
  OBJ palette RAM is a better renderer interface than independently reproducing
  palette allocation.
- Compressed graphics have already been materialized into OBJ VRAM by the time
  they need to be drawn.

Expansion is not an expanded-viewport implementation. At the reviewed revision
it still uses a 240x160 display, `MAP_OFFSET == 7`, 16 object events, 64 sprites,
and essentially the same object spawn/cull envelope as Emerald. Its main value
here is a compatibility test corpus and a source of design cases, not a renderer
to port wholesale.

## 5. merrp and other fork findings

[merrp](https://github.com/PokemonSanFran/merrp), reviewed on branch
`followers-expanded-id` at `88cfd9e` (2024-07-11), contains earlier work on
followers, large/asymmetric overworld sprites, dynamic overworld palettes, and
reflections. It retains the normal object spawn/cull model and does not provide
an arbitrary viewport renderer.

For this project, current pokeemerald-expansion is the better behavior reference:
it is maintained, integrates more of these features together, and supplies a
broader stress case. merrp remains useful for tracing feature provenance or
older implementation intent. Neither project eliminates the need for a generic
native sprite path or wider object streaming.

## 6. Existing widescreen/expanded-view work found

Searches covered GitHub repositories and code for combinations of
`pokeemerald`, `widescreen`, `expanded viewport`, `extended camera`, `larger
framebuffer`, `native renderer`, `SDL renderer`, `field renderer`, and
`arbitrary resolution`. No existing Emerald decomp project was found that
renders a genuinely larger live overworld while leaving gameplay authoritative.
This is a search result, not proof that no private or unindexed experiment
exists.

The closest public work was:

- [pokeemerald-multiplatform](https://github.com/gradenGnostic/pokeemerald-multiplatform),
  the upstream lineage of this repository: it presents the fixed GBA frame with
  scaling/borders rather than revealing more world.
- [EmeraldRecomp](https://github.com/mstan/EmeraldRecomp): a static recompilation
  project whose PPU model remains GBA-oriented; no true expanded overworld path
  was found.
- [pokeemerald_SDL3](https://github.com/toucans/pokeemerald_SDL3) and similar
  native ports: useful host-platform precedents, but still fixed-frame ports in
  the areas relevant to this research.
- Porymap and related map editors/fangame tooling: able to render large static
  map regions, but not live Emerald presentation state.

The local `gba_easy_draw.c` and `gba_fast_draw.c` implementations are closer to
what is needed for exact blend/window/OBJ rules than any external widescreen
project found. Their algorithms should be factored behind arbitrary target
bounds after the map/sprite state adapter is stable.

## 7. Recommended map-rendering approach

Use a completely native tilemap renderer, informed by Porymap but driven by
Emerald runtime data. This is option D with selective reuse of option C; it is
not a port of Porymap and not margin reconstruction.

For each visible 8x8 world tile:

1. Resolve its map/metatile coordinate.
2. Prefer a matching entry from the live BG ring when available.
3. Otherwise query the mutable map grid and layer type.
4. Select primary or secondary metatile storage at ID 512.
5. Map split/covered/normal metatile halves to BG3/BG2/BG1 exactly as
   [`field_camera.c`](../src/field_camera.c) does.
6. Apply tile flips and palette bank and sample current BG VRAM/PLTT.
7. Composite with exact GBA BG priority/transparency rules.

The live BG ring is an override, not an authoritative center image. This matters
because calls such as door drawing can write final tile entries directly into
the ring, whereas normal runtime tile replacements do update the backup map
grid. A later audit should catalog all direct overworld BG writes and decide
whether each is naturally captured by the ring or needs a native visual-event
hook.

Cache decoded 8x8 index data or resolved tile entries, not final colors: weather,
fades, and dynamic palette operations can change colors without changing map
geometry. Retain a CPU path as the correctness oracle even if a GPU atlas path
is added.

## 8. Recommended sprite-rendering approach

The alternatives have different failure modes:

| Source | Strength | Fatal limitation by itself |
|---|---|---|
| Final OAM + OBJ VRAM | Closest to final GBA output; includes animation, subsprite expansion, priorities, affine state | Coordinates wrap, invisible/off-screen objects may be omitted, and world ownership is lost |
| `gSprites` directly | Signed logical coordinates, animation/subsprite state, callback/effect identity, priority | Requires reproducing OAM emission and distinguishing screen culling from intentional invisibility |
| Object events + source sheets | Stable map coordinates and semantic identity | Misses generic field/weather sprites, dynamic frames, palette allocation, transforms, and transient offsets |
| Another fork's renderer | No complete implementation was found | Does not solve the runtime/view problem |

Use a hybrid of the first two, with object events as metadata:

- Factor sprite/subsprite emission before hardware coordinate truncation and
  off-screen removal.
- Preserve signed logical positions (`x + x2`, `y + y2`, coordinate offsets),
  affine state, flips, OBJ mode, priorities, and emission/OAM order.
- Read the current image from OBJ VRAM and current colors from OBJ PLTT. Do not
  reproduce sprite animation or decompression in the native renderer.
- Associate commands with an object event when one exists so an object culled
  only for the 240x160 screen can still be drawn in the expanded view.
- Render unowned in-use field sprites generically. Reflections, surf blobs,
  shadows, grass effects, many weather particles, and other linked effects then
  do not need callback-specific renderer code.
- Reproduce GBA ordering precisely: BG and OBJ priority, OBJ-vs-BG tie behavior,
  lower OAM index on sprite ties, subsprite order, affine transforms, blend,
  windows, and mosaic as capabilities are added.

Reading final OAM remains useful as a 240x160 parity oracle and may seed exact
draw order, but it cannot be the only expanded-view input.

## 9. Object-loading limitations

Vanilla spawning in [`event_object_movement.c`](../src/event_object_movement.c)
uses this inclusive map-coordinate envelope relative to the camera position:

```text
x: cameraX - 2 through cameraX + MAP_OFFSET_W + 2  (= cameraX + 17)
y: cameraY     through cameraY + MAP_OFFSET_H + 2  (= cameraY + 16)
```

Removal uses the same bounds for both current and initial coordinates. A
separate sprite-offscreen update tests against 240x160 pixel bounds plus a small
padding and sets `sprite->invisible`. The current native margin renderer can
override the latter for an already active object, but it cannot draw an object
that was never instantiated or has been destroyed.

Consequences:

- The range is asymmetric and tied to the original camera/grid pipeline, not a
  requested viewport rectangle.
- `OBJECT_EVENTS_COUNT` is 16 and `MAX_SPRITES` is 64. Distant objects can consume
  slots needed nearby; shadows, reflections, followers, and weather consume
  additional sprite slots.
- Object events live in `SaveBlock1`. Simply changing the count changes save
  layout unless storage and serialization are deliberately redesigned.
- Active NPCs run AI/movement and can participate in collision, trainer checks,
  and scripts. Extending the active range is not presentation-only and can
  change observable gameplay or deterministic timing.
- `TrySpawnObjectEvents` iterates the current map's event templates. Events on a
  visible connected map do not exist until the connection transition changes
  the current map. Enlarging the numeric range does not fix that.

Therefore the renderer must expose coverage truth instead of promising all
objects at every zoom. The long-term design decision should be explicit:

1. Safest initial policy: support expanded terrain but limit released viewport
   presets to ranges whose missing-object behavior has been tested and disclose
   that connected-map NPCs are not streamed.
2. Faithful long-term policy: add a host-side object streaming layer with enough
   capacity, prioritize proximity, preserve save compatibility, and separate
   simulation/interaction activation from presentation coverage wherever game
   semantics allow it.
3. Presentation proxies can render static templates from connected maps, but
   they cannot truthfully invent NPC AI/animation state and should not masquerade
   as a complete solution.

Option 2 is the credible final route, but it is gameplay-adjacent and deserves
its own design/replay tests after renderer parity. Connected-map events require
multi-map event loading plus coordinate transforms, not merely larger constants.

## 10. Map-data and loading limitations

Emerald's backup grid is more capable than the current GBA viewport suggests.
[`fieldmap.c`](../src/fieldmap.c) allocates 10,240 metatile entries, places the
current map at a seven-metatile offset, copies cardinal connection strips around
it, and returns a repeating 2x2 border outside valid data. Runtime calls to
`MapGridSetMetatileIdAt` mutate this grid, so normal changed tiles are visible to
a native query.

The connection apron is seven metatiles north, south, and west, and eight east.
Consequently:

- 300x200 adds 30x20 pixels per side and 360x240 adds 60x40. Both fit inside the
  existing apron geometrically.
- 480x320 adds 120x80 per side. The horizontal margin is 7.5 metatiles and can
  exceed the seven-metatile side, so a resolver beyond the backup grid is needed.
- Large outdoor maps, caves, buildings, Pokémon Centers, and most indoor maps
  use the same layout/grid/tileset machinery. Maps without connections correctly
  fall back to their border.
- Dynamically generated layouts such as Battle Pyramid/Trainer Hill and special
  transitions need explicit test coverage; they should initially request
  fallback if the snapshot cannot describe them.
- Connected terrain in the backup grid is a copy made during map loading. It is
  sufficient for normal visual continuity, but an inactive connected map is not
  a second live simulation and its events/runtime changes are not resident.

For the first two expanded presets, the backup grid is enough for terrain. For
larger/asymmetric views, add a world resolver using map connections and source
layouts, with border fallback and loop/overlap protection. Do not globally
increase `MAP_OFFSET` merely to render more terrain; that couples presentation
size to core map allocation and still does not solve connected NPC state.

## 11. Weather and field-effect strategy

Split weather into color state, sprite state, and screen-space effects.

- Palette-based weather/fades: consume final palette RAM after the frame's
  palette transfer. This naturally captures many dynamic palette effects.
- Sprite-based particles/effects: use the generic sprite stream. Existing rain,
  snow, ash, bubbles, reflections, shadows, surf, and field-effect sprites can be
  drawn without type-specific pixel code once their live commands are available.
- Pattern extent: many weather systems create only enough particles/cloud
  sprites for 240x160. The native renderer must eventually provide a
  weather-specific extension policy that repeats or synthesizes presentation
  outside the GBA rectangle without altering weather simulation.
- BG0, windows, blend registers, HBlank/scanline effects, and screen-space fog
  require explicit compositor capabilities. Until implemented, they trigger a
  full GBA fallback.

Do not extend weather by spawning extra gameplay sprites. Add native
presentation providers keyed by the already authoritative weather state. This
keeps the visual density stable across viewport sizes and does not consume GBA
sprite slots.

## 12. Camera strategy

Keep Emerald's camera unchanged. Capture a signed/fixed-point world top-left
from `gSaveBlock1Ptr->pos` and `GetCameraOffsetWithPan`, then center the requested
logical viewport on the same focus as the 240x160 view. Preserve camera panning,
screen shake, sprite coordinate offsets, and transition state as separate
snapshot fields rather than modifying player coordinates.

The current origin formula—map position times 16 plus camera offset, minus the
expanded margin—is fundamentally correct. Generalize it from a fixed zoom enum
to `{logicalWidth, logicalHeight}` and define rounding for odd differences. Use
one coordinate convention at API boundaries (world pixels at the upper-left of
the 8x8/metatile grid) and test negative offsets and connection transforms.

During map transitions, the existing backup-grid saved-view movement and live BG
ring should supply center continuity. Any transition whose needed state cannot
be represented should request fallback rather than guessing.

## 13. Fallback strategy

`DrawFrame` remains the universal renderer for title, intro, battles, menus,
non-overworld scenes, unsupported cutscenes, and unsupported GPU/weather effects.
Initially, dialogue/windows over the field should fall back for the whole frame.
Extracting and compositing a transparent GBA UI layer can be a later project.

Selection should be based on scene identity plus required capabilities, not
exact callback pointer allowlists. Log one concise reason when native rendering
falls back, with rate limiting. Keep the user's viewport/zoom selection while a
scene is in fallback so the native view returns after the scene ends.

At no point should failure to build a snapshot corrupt gameplay state. The
fallback path must be callable without undoing or replaying a game update.

## 14. Licensing implications

| Project | License finding at reviewed revision | Reuse decision |
|---|---|---|
| This repository | Root license grants MIT terms only to the multiplatform-port modifications and explicitly excludes upstream game/assets and third-party work | New native renderer code can follow this scoped license; preserve upstream/asset provenance |
| Porymap | LGPL-3.0 (`LICENSE.md`; GitHub also identifies LGPL-3.0) | Direct adaptation is possible only with LGPL notices, source/modification and relinking obligations as applicable. Prefer independent implementation of the general algorithms to avoid mixing Qt/LGPL code into the port |
| pokeemerald-expansion | No `LICENSE` or `COPYING` file was found; README asks for RHH/contributor credit, which is not a copyright license | Reference behavior and independently reimplement ideas; do not copy/adapt code without permission or verified per-file provenance |
| merrp | No license file was found | Reference only; do not copy/adapt code without permission |
| EmeraldRecomp | No license file was found in the reviewed repository | Reference only |
| pokeemerald_SDL3 | GitHub reports an MIT license | Code may be reusable under MIT with its copyright/license notice, but no needed expanded-renderer implementation was found |

Ideas, data-flow patterns, and independently derived algorithms are not the same
as copying expression. Keep a short provenance note for every external design
consulted, and perform a file-level license check before importing any code.
Porymap source should not be pasted into this repository as "reference" code.
Likewise, a credit request in a README does not substitute for reuse permission.

## 15. Current zoom code to retain

Retain or evolve these pieces:

- The fallback-first integration point in desktop video and preservation of the
  authoritative GBA renderer.
- Zoom input/state separation in `native_overworld_zoom.c`, replacing its fixed
  enum later with validated logical viewport presets/configuration.
- Signed floor-division and positive-modulo helpers and the tested camera-origin
  math.
- The exact split/covered/normal metatile-layer mapping and
  primary/secondary-tileset selection.
- Sampling current BG/OBJ VRAM and palettes, which automatically carries much of
  the live animation/palette state.
- The second SDL texture/presentation route, generalized to dynamically sized
  textures and arbitrary aspect ratios.
- Existing small unit-test seams for layer selection, border lookup, sampling,
  off-screen coordinates, input, and fallback.
- The readiness predicate's intent: native rendering must be fail-closed. Its
  implementation should evolve into capability reporting.

## 16. Current zoom code to remove or replace

After the replacement renderer reaches parity—not during initial prototyping—
remove or replace:

- The 240x160 center `memcpy` and all "skip authoritative center" branches.
- Margin-only object rendering and callback-specific recognition of only shadow
  and player surf sprites.
- Fixed maximum image dimensions and hard-coded 300x200/360x240 assumptions.
- Approximate `(priority << 8) | subpriority` compositing where it diverges from
  GBA BG/OBJ/OAM ordering.
- The exact callback/register checklist in
  `Overworld_IsNativeExpandedRendererReady`; retain only scene/capability checks.
- Forced 3:2 destination sizing for a native widescreen logical viewport.
- Per-pixel repeated map metadata work once correctness permits tile/metatile
  caching.

Keep the old compositor behind a development flag until the 240x160 parity suite
and fallback telemetry show the new path is reliable. Then delete it in a
separate, reviewable change.

## 17. Staged implementation plan

### Stage 0: Define the contract and parity harness

- Specify `NativeOverworldSnapshot`, viewport coordinates, capture timing, and
  capability/fallback reasons.
- Add an offline frame-diff/debug view that can compare native and GBA BGR555
  outputs and classify discrepancies by BG/OBJ/effect.
- Freeze the current compositor as a temporary reference; do not expand it.

Exit criterion: deterministic snapshots and repeatable pixel diffs without
changing game state.

### Stage 1: Render the complete 240x160 map natively

- Render BG3/BG2/BG1 across the entire frame with no `DrawFrame` pixels.
- Use mutable map-grid data, live BG-ring overrides, BG VRAM animation frames,
  and final palettes.
- Exclude or mask OBJ pixels in comparisons at first.

Exit criterion: representative outdoor, cave, building, Pokémon Center,
animated-water, runtime-tile-change, door, and border cases match the GBA map
background.

### Stage 2: Establish map parity and transitions

- Fix priority, scrolling, pan/shake, border/connection, tile-animation timing,
  door/transient BG writes, fades, and ordinary map transitions.
- Build regression captures rather than relying on visual inspection alone.

Exit criterion: zero or explicitly understood/map-independent pixel differences
for the supported capability set.

#### Stage 2.1: Moving-frame scroll parity

The map-background renderer must present the frame the GBA is PRESENTING, not the
newest logical camera state. Emerald latches `REG_BG*HOFS/VOFS` in the VBlank
handler (`FieldUpdateBgTilemapScroll`) after `OverworldBasic` has advanced the
logical camera, so on a moving frame the scroll is one frame of motion behind
`GetCameraOffsetWithPan()`. The snapshot therefore carries two origins:

- **LOGICAL** `cameraMapX*16+cameraX` — the game's model of where the window
  points; the grid/border/connection fallback resolves in this space.
- **PRESENTATION** the latched BG scroll registers — where the GBA physically
  samples the 32x32 text-mode ring (`(scroll + sx) & 0xFF >> 3`). The native ring
  lookup samples here; the `gba_easy_draw.c` oracle reproduces exactly this.

`HOFS/VOFS == cameraX/cameraY` only on stationary frames. A UNIFORM scroll that
differs from the camera is NORMAL (moving frame) and must not fall back; only an
internally inconsistent latch (the three layers disagree) is a torn presentation
and is rejected by the capture gate. Never patch the camera to force equality.

Two dev-only mechanisms support offline verification (no game run):

- `POKEEMERALD_NATIVE_PARITY_SCROLL_FIXTURES=N` serializes up to N
  bg-scroll-divergent compared frames under `build/native-parity/scroll/` as
  offline fixtures (snapshot + native + gba oracle + manifest), independent of
  parity success — a real captured moving frame can then be reproduced from its
  snapshot alone against the real oracle.
- A rate-limited `PARITY SCROLL` diagnostic (parity enabled only) logs the
  logical camera and the three BG scroll latches side by side; moving-frame
  frames log every time, stationary frames at most once per 60 capture frames.

One manual runtime run validated this end-to-end: 859/859 compared real gameplay
frames were pixel-exact (0 mismatch frames, 0 mismatch pixels). Two real
moving-frame captures (frames 218/219, a 1px horizontal step) are committed under
`tests/fixtures/native-parity/scroll/` and reproduced offline by
`tests/native_overworld_scroll_fixture_unit.c` (re-render == captured native
bit-for-bit, == captured GBA oracle with 0 mismatches, divergence asserted).
During that run the parity skip histogram showed 8 `unknown` reasons: the
`NATIVE_FALLBACK_BG0_OVERLAY` label was missing from `FallbackName()` — a
legitimate BG0-gate rejection, not a capability gap. The label was added and a
module test now pins every fallback reason to a non-`unknown` name.

### Stage 3: Add a generic native sprite path at 240x160

- Factor pre-wrap sprite/subsprite draw-command emission.
- Add exact normal OBJ priority/order, flips, affine objects, OBJ blend, and
  linked field sprites in measured increments.
- Validate player, NPCs, trainers, shadows, surf, reflections, grass effects,
  large/asymmetric sprites, and dynamic palettes.

Exit criterion: full supported 240x160 overworld parity with the center copy
disabled.

### Stage 4: Expand terrain and existing live sprites

- Generalize the SDL texture and camera to 300x200 and 360x240.
- Use only the existing seven-metatile backup apron initially.
- Add a debug overlay/telemetry for the requested world rectangle versus map and
  object-event coverage. Do not claim complete NPC coverage yet.

Exit criterion: seamless map expansion with honest detection of unavailable
object state and unchanged 240x160 behavior.

### Stage 5: Design and extend object/map streaming

- Decide save-compatible host storage and slot/capacity policy.
- Separate rendering coverage from interaction/simulation activation to the
  extent Emerald semantics permit; add deterministic behavior tests.
- Load/transform connected-map event templates and define transition ownership.
- Add a direct connection resolver for views beyond the backup apron.

Exit criterion: every object visible in supported expanded presets has real,
authoritative state, without early trainer/script/collision side effects or
save-format regressions.

### Stage 6: Extend weather, field effects, and UI policy

- Fill wider views with native weather presentation providers.
- Add remaining blend/window/BG0/scanline capabilities selectively.
- Keep full fallback for unsupported effects; evaluate native-world plus GBA-UI
  composition only as a separate feature.

Exit criterion: a documented capability matrix with tested fallback for every
weather/field-effect family.

### Stage 7: Runtime zoom, widescreen, and optimization

- Add 480x320 and aspect-driven/16:9 logical viewport experiments only after
  connection and object coverage exist.
- Define sensible logical-resolution caps, pixel scaling, and odd-margin rules.
- Profile, then add tile caches, dirty-region updates, or a GPU backend while
  retaining the CPU parity oracle.

Exit criterion: runtime switching is stable through transitions/fallback and
each advertised viewport reveals legitimate, fully populated world state.

## 18. Main technical risks

Ordered roughly by severity:

1. **Object streaming changes gameplay.** More active objects consume fixed
   slots and run logic earlier; the count is embedded in save structures.
2. **Connected-map NPC state does not exist.** Terrain connections and event
   connections are different problems.
3. **Presentation state is not all in the map grid.** Doors, BG-ring writes,
   BG0, windows, and scanline effects need capture or explicit support.
4. **Exact OBJ composition is intricate.** Affine/subsprite geometry, hardware
   coordinate wrapping, OAM tie order, blend, mosaic, and intentional
   invisibility must be separated from screen culling.
5. **Weather is authored for 240x160.** Drawing existing sprites is insufficient
   to fill a wider screen consistently.
6. **Capture timing can tear.** Map, sprite, VRAM, palette, and register values
   must describe one presentation frame.
7. **Transitions and special maps.** Dynamic maps and cutscene camera behavior
   may require fallback or new snapshot data.
8. **Licensing/provenance.** The most feature-rich external Emerald forks do not
   presently offer clear copy permission.
9. **Premature optimization.** A fast but approximate GPU path would recreate
   the current seam/debugging problem at a larger scale.

## 19. Smallest credible prototype

The smallest credible prototype is not 300x200. It is a complete, independent
240x160 **map-background** renderer with no center copy.

It should:

- Capture camera, mutable backup grid, final BG1/BG2/BG3 ring entries, current
  BG VRAM, palette RAM, and relevant BG registers at one defined frame point.
- Render every 240x160 background pixel through the native path.
- Compare against `DrawFrame` with OBJ pixels disabled/masked and produce a
  mismatch count plus an inspectable diff.
- Exercise one outdoor map with animated water, one cave, one building, one
  Pokémon Center, a border, a cardinal connection, a runtime-modified metatile,
  a door/transient tilemap write, camera pan, and an ordinary map transition.
- Fall back without changing state for any scene outside the prototype's
  capability set.

Success means the map renderer is demonstrably independent and faithful. Only
then should the project add generic sprites at 240x160, and only after full
240x160 parity should expanded presets be treated as product behavior.

## Bottom line

Proceed with the bolt-on native presentation backend. Do not port Porymap, do
not widen `MAP_OFFSET` as the architecture, and do not reconstruct sprites from
source sheets. Reuse Emerald's live decisions through a snapshot, live VRAM,
palettes, final BG-ring overrides, and a pre-wrap generic sprite command stream.
Treat wider object streaming as its own gameplay-sensitive milestone. This path
supports arbitrary viewports without making the native renderer responsible for
Emerald's game logic and preserves `DrawFrame` as a reliable universal fallback.

## Research sources and scope

Local source audit was performed against repository revision
`1154794b` and included:

- [`src/platform/native_overworld_renderer.c`](../src/platform/native_overworld_renderer.c),
  [`src/platform/native_overworld_zoom.c`](../src/platform/native_overworld_zoom.c),
  [`src/platform/desktop_video.c`](../src/platform/desktop_video.c), and the
  readiness predicate in [`src/overworld.c`](../src/overworld.c).
- [`src/fieldmap.c`](../src/fieldmap.c),
  [`src/field_camera.c`](../src/field_camera.c),
  [`src/event_object_movement.c`](../src/event_object_movement.c),
  [`src/field_weather.c`](../src/field_weather.c),
  [`src/field_effect.c`](../src/field_effect.c), and
  [`src/tileset_anims.c`](../src/tileset_anims.c).
- Map/object/sprite definitions in
  [`include/global.fieldmap.h`](../include/global.fieldmap.h),
  [`include/fieldmap.h`](../include/fieldmap.h),
  [`include/sprite.h`](../include/sprite.h), and
  [`include/constants/global.h`](../include/constants/global.h).
- Existing GBA presentation implementations in
  [`src/platform/gba_easy_draw.c`](../src/platform/gba_easy_draw.c) and
  [`src/platform/gba_fast_draw.c`](../src/platform/gba_fast_draw.c).

External repositories were shallow-cloned for read-only inspection at these
revisions:

- Porymap
  [`26b919d7ff4b54152de010abd5bf344af2ebe116`](https://github.com/huderlem/porymap/tree/26b919d7ff4b54152de010abd5bf344af2ebe116),
  especially
  [`src/core/maplayout.cpp`](https://github.com/huderlem/porymap/blob/26b919d7ff4b54152de010abd5bf344af2ebe116/src/core/maplayout.cpp),
  [`src/core/map.cpp`](https://github.com/huderlem/porymap/blob/26b919d7ff4b54152de010abd5bf344af2ebe116/src/core/map.cpp), and
  [`src/ui/imageproviders.cpp`](https://github.com/huderlem/porymap/blob/26b919d7ff4b54152de010abd5bf344af2ebe116/src/ui/imageproviders.cpp).
- pokeemerald-expansion
  [`4189835135f8af5a4754167b18130d411a9c96ad`](https://github.com/rh-hideout/pokeemerald-expansion/tree/4189835135f8af5a4754167b18130d411a9c96ad),
  especially its overworld configuration, event-object movement, field effects,
  weather, follower, and palette code.
- merrp
  [`88cfd9e2673e462bd1f0bee058c20443ccadda3b`](https://github.com/PokemonSanFran/merrp/tree/88cfd9e2673e462bd1f0bee058c20443ccadda3b),
  particularly the follower/expanded-ID branch's object movement, graphics,
  dynamic palette, and reflection work.

Repository/code searches were also performed for the terms summarized in
section 6. No game executable was launched, no release artifact was read or
modified, and no external repository code was copied into this repository.
