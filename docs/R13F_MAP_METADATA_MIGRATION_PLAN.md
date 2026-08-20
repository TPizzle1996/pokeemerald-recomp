# R13-F — Map Metadata & Event Structure Migration Plan

Stage scope: migrate **map metadata and map-event structure only** — map
headers, layout metadata, event bundles (object-event templates, warps, coord
events, bg events), connections, and map routing tables — to ROM-owned
resources, published through a new `EmeraldMapCompat` seam. **Design/implementation
review only — no production code modified, no commit. R13-G (field scripts)
not started.** R11 block/border/tileset payloads are NOT re-migrated (already
ROM-owned). Map script bytecode and NPC dialogue scripts are **NOT in F** (they
are R13-G).

Grounding: qualified reference ELF/ROM (SHA-1 `f3ae0881…07b7`), R13-A
inventory, R11 native-world neighborhood, R13-E2 map-key identity. Current
proven state: pack 18,971 entries / 13,987,040 B; cap 32,768; ranges
5,851/8,192.

---

## 1. Exact map metadata inventory (R13-A baseline; agents refine)

| Family | Count | Canonical ROM bytes | Native bytes | Pointer-bearing? | Pointer targets |
|---|---|---|---|---|---|
| Map headers (`gMapHeaders` / `sMapHeaders`) | 518 | 518 × 28 (GBA) | 518 × 40 (native) | 4 ptrs/row | layout, events, scripts, connections |
| Map layouts (`gMapLayouts`) | 441 | 441 × 24 (GBA) | 441 × 32 (native) | 4 ptrs/row | border, blockdata, primary tileset, secondary tileset (R11-owned) |
| Object-event templates | 2,776 | 2,776 × 24 (GBA) | 2,776 × 32 (native) | 1 script ptr, 1 movement field | R13-G script, R13-B movement |
| Warp events | 1,313 | 1,313 × 8 (GBA) | 1,313 × 8 (native) | 0 | pure data |
| Coord events | 375 | 375 × 16 (GBA) | 375 × 16 (native) | 1 script ptr | R13-G script |
| BG events | 720 | 720 × 12 (GBA) | 720 × 24 (native) | 1 script ptr | R13-G script |
| Connections | 148 | 148 × 12 (GBA) | 148 × 12 (native) | 0 | pure data |
| Region-map entries | 213 | 213 × 16 (GBA) | 213 × 16 (native) | 0 | routing/gfx |
| Map routing tables (`gMapGroups`, `gMapLayouts` ptrs) | — | ~2,208 | ~2,208 | ptrs (to layouts) | R11-owned |
| **Total** | — | **~180,977 B** | — | — | — |

(Exact counts and bytes are pinned from the qualified ROM during implementation;
the R13-A audit sizes are the baseline.)

## 2. Parity / revision-delta gate

Compare current recomp map metadata against the qualified vanilla ROM for every
header, every layout, and a representative set of event bundles. Expected:
layout/packing-only differences (GBA 4-byte padding vs native packing, pointer
4→8 width), and the R13-B movement-script pointer difference (the movement
family is compiled with the recomp's COMPILED_PENDING_MIGRATION leaf data).
Map content (names, music, weather, event coordinates, warp targets,
connection data) must be byte-identical. Any C/D divergence = STOP that family.

## 3. Stable map identity (shared with R13-E2)

R13-E2 established semantic map keys: `MAP_ROUTE101` → `route-101`,
`MAP_METEOR_FALLS_1F` → `meteor-falls-1f`, `MAP_ALTERING_CAVE` →
`altering-cave-<wildset>`. R13-F uses the **identical** map-key component
(`emerald:data/map/<map>`). The mapGroup/mapNum → semantic name bijection is
proven via the map constants + the E2 encounter key derivation (no second
naming scheme). MAP_UNDEFINED (0xFF) has no resource; synthetic/unused maps are
documented.

## 4. Resource granularity

Per-map resources, with event sub-resources where the event arrays are
mod-targetable:

```
emerald:data/map/<map>/header           (map header, the 4-pointer GBA row)
emerald:data/map/<map>/layout-meta       (MapLayout metadata, ptrs to R11-owned)
emerald:data/map/<map>/events            (event bundle or per-type sub-resources)
emerald:data/map/<map>/connections       (connection array, if any)
```

Inside events, the model is **one per-map event bundle** (the MapEvents struct
with object/warp/coord/bg arrays packed together per map). This avoids
thousands of tiny per-event resources while keeping the mod contract: "change
one warp on Route 101" → override the Route 101 events bundle. A future
per-event refinement can be layered on top without breaking the bundle format
(per-event sub-resources can be extracted from the bundle provenance). The
canonical payload is the exact ROM slice of the MapEvents block (contiguous in
the event data region).

## 5. Map header pointer graph

```
MapHeader
  ├─ MapLayout          → R11 tilesets/blocks/borders
  ├─ MapEvents          → R13-F (event bundle)
  ├─ MapScripts        → R13-G (script bytecode pointer)
  └─ MapConnections    → R13-F (connection array)
```

R13-F publishes the header, layout metadata, events, and connections. The
**script pointer** (MapHeader.scripts) stays as a **GBA logical address** in
the canonical payload and is resolved through `HostResolveGbaAddr` until R13-G
replaces it with a native script-arena pointer (the existing
`HostResolveGbaAddr` bridge works for `GbaAddr` script data that the engine
already resolves). At publication, the header's script pointer is retained as
validated provenance bytes and the native header's script pointer continues to
resolve through the existing GBA-logical-address machinery (unchanged until G).

## 6. Layout metadata vs R11 ownership

R11 already owns tilesets, map blocks, borders, and the native world
neighborhood. The MapLayout metadata row carries 4 pointers to R11-owned
content: border, blockdata, primary tileset, secondary tileset. R13-F
publishes the structural MapLayout metadata (the 24-B GBA row) and **rebuilds
the native pointers** to the existing R11-published resources (via the
`EmeraldLayoutCompat` / `EmeraldTilesetCompat` query APIs). No blockdata /
border / tileset payload is duplicated.

## 7. Event metadata model

| Event type | GBA wire size | Native size | Script pointer? | Movement field? | Other pointers |
|---|---|---|---|---|---|
| ObjectEventTemplate | 24 B | 32 B | yes (script GBA addr) | yes (moveType/script ptr) | none |
| WarpEvent | 8 B | 8 B | no | no | none |
| CoordEvent | 16 B | 16 B | yes (script GBA addr) | no | none |
| BgEvent | 12 B | 24 B | yes (script GBA addr) | no | none |

The **script pointer** fields in object/coord/bg events stay as GBA logical
addresses in the canonical payload and remain resolved through
`HostResolveGbaAddr` until R13-G. These are the **precise F→G boundary
contract** (§16).

## 8. Object-event movement interaction (R13-B)

R13-B migrated movement scripts additively (COMPILED_PENDING_MIGRATION). The
object-event's `moveType` field is a movement-script pointer. **Recommendation:
R13-F is NOT the place to cut over movement scripts.** The movement family's
leaf seam (`EmeraldLeafCompat`) is additive (DEGRADE-class, not REFUSE), and
the movement resources are COMPILED_PENDING_MIGRATION (not ROM_BASE_ONLY). The
movement cutover is a separate decision (the R13-B leaf family's
live-cutover is a follow-up wave, not an R13-F dependency). R13-F keeps the
movement pointer as validated provenance (GBA address) in the canonical
payload; the native field continues to resolve through the compiled movement
table until the leaf wave completes.

## 9. Text interaction

Map metadata fields do NOT directly point to R13-C text resources. NPC/sign
dialogue is in scripts (R13-G). No R13-F text flip is needed.

## 10. Connections

Per-map connection arrays (direction, connected mapGroup/mapNum, offset).
Canonical payload = exact ROM slice. Published along with the map header's
connections pointer (rebuilt to the published connection arena). The seam
validates every connected map target exists.

## 11. Region-map metadata

The 213 region-map entries are **DEFERRED from R13-F**. They depend on region
map graphics (not yet resource-owned), are largely routing/index data, and
are not load-bearing for the map-header/layout/event/connection graph.
Classified as INDEX_ROUTING (deferred to a later map-aux or gfx stage).

## 12. gMapGroups / gMapLayouts routing

These are INDEX_ROUTING tables: gMapGroups maps group→num arrays for the
linear map-index lookup; the native world neighborhood indexes them directly.
**Keep them compiled as engine routing** (not migrated in F). The map metadata
resources are keyed by semantic name, not index, so the routing tables stay as
engine-owned lookup helpers (consistent with how the species→dex routing tables
were kept compiled in D1).

## 13. Native publication seams

One `EmeraldMapCompat` seam with internal subfamilies (header, layout-meta,
events, connections). Transactional phase-1 validates every resource (schema,
size, ROM_BASE winner, generated set equality, the script pointer GBA addresses
are valid, the layout pointers bind to existing R11 resource identities, every
connection target map exists). Phase-2/3 publishes the native HOST_DATA
header/layout/event/connection arrays, rebuilding the native pointers to the
R11-published resources, to the R13-G GBA-logical script bridge, and to the
event/connection arenas. Atomic, REFUSE-class.

Publication order: (1) layout metadata, (2) event bundles, (3) connections,
(4) map headers, (5) native-world neighborhood rebind.

## 14. Native world neighborhood interaction

The R11 native world neighborhood currently accesses compiled `gMapLayouts`,
`gMapGroups`, `gMapHeaders` by mapGroup/mapNum linear scan (src/platform/
native_world_neighborhood.c, src/platform/native_overworld_renderer.c,
src/platform/native_field_compositor.c). R13-F publishes these tables as
HOST_DATA fill targets with the **same symbols and native layouts** — zero
consumer edits. The neighborhood code keeps indexing the same symbols; the
only change is the byte source (compiled → pack-published). The R11 layout/
tileset/block pointers are rebuilt to the existing R11-published resources
by the seam, so the neighborhood's render path resolves them identically.

## 15. State-v5

**Correction vs R13-A §6:** the R13-A audit said "map/trainer/encounter tables
have zero serialized pointer surface." This is true for the **compiled tables**
themselves (the 518 header rows, 441 layout rows — they are `.rodata` and not
in any serialized slice). However, the **EWRAM `gMapHeader` working copy**
(`src/fieldmap.c:29`, `EWRAM_DATA struct MapHeader gMapHeader = {0}`) has **4
native pointers** (mapLayout, events, mapScripts, connections) that are
serialized in the `STATE_SECTION_EWRAM` slice. The State-v5 walker already
captures these as sidecar records against the current compiled ranges.

After R13-F publishes the metadata tables, these 4 pointers will point into the
published HOST_DATA arrays. The **published arrays need COMPAT_OBJECT range
registration** (one per published family: map headers, layouts, events,
connections ≈ 4 ranges). The walker will capture the same pointers with the
new F resource keys (a benign key migration — the pointer values are the same,
only the resource identity changes). Post-F range count ≈ **5,855 / 8,192**
(5,851 + 4). No format change, no cap raise.

## 16. Script boundary / R13-G contract (precise)

Every field that remains script-owned after F:

| Field | Pre-F state | Post-F state | R13-G action |
|---|---|---|---|
| MapHeader.scripts | compiled GBA addr, resolved via HostResolveGbaAddr | canonical = GBA addr (validated provenance), native = HostResolveGbaAddr bridge | G replaces with native script-arena pointer |
| ObjectEventTemplate.script | same | same | G replaces |
| CoordEvent.script | same | same | G replaces |
| BgEvent.script | same | same | G replaces |
| ObjectEventTemplate.moveType | compiled movement ptr → EmeraldLeafCompat | canonical = GBA addr (provenance), native = compiled movement bridge | R13-B leaf live-cutover wave replaces |
| MapEvents (event struct) | compiled struct | published native struct | unchanged |
| NPC dialogue | in scripts | in scripts | G owns |

R13-G can pick up every script-pointer field without changing R13-F resource
identity (the canonical payload retains the GBA address as provenance; G
swaps the native resolution bridge).

## 17. ROM-hack import model

| Mod | Mechanism | Class |
|---|---|---|
| change one warp on Route 101 | override `emerald:data/map/route-101/events` (warp array) | direct F override |
| add/remove an NPC | override `emerald:data/map/<map>/events` (object-event array) | direct F override |
| change NPC movement type | override the event bundle (object-event moveType field) | direct F override |
| change map connection | override `emerald:data/map/<map>/connections` | direct F override |
| change map weather/music/type | override `emerald:data/map/<map>/header` | direct F override |
| change layout metadata | override `emerald:data/map/<map>/layout-meta` | direct F override |
| change blockdata | override the R11 `emerald:layout/blockdata/<map>` resource | R11 override |
| change NPC dialogue/script | override the R13-G script resource | R13-G override |
| new map | new `emerald:data/map/<new>` resources | direct (schema unchanged) |

## 18. Pack arithmetic

Current 18,971 / cap 32,768. R13-A projection ≈ 700 map bundles/resources.
Taking per-map (header + layout-meta + events + connections) = 518 maps ×
~4 resources ≈ 2,072 resources. But many maps have no connections or no events,
so the actual count is lower: ~518 headers + ~441 layouts + ~518 event bundles
+ ~148 connection sets ≈ 1,625 resources. Canonical bytes ≈ 180,977 (header
28 B + layout 24 B + events + connections). Projected pack ≈ **20,600 entries**
(well under cap). No cap change.

## 19. Isolation

- Compiled map headers absent (symbol sweep + whole-table hash).
- Compiled map layouts absent (symbol sweep; R11-owned pointers remain in the
  published layout table).
- Compiled event bundles absent (symbol sweeps per event type).
- Compiled connection arrays absent.
- Compiled map routing tables absent where migrated.
- Every pointer to R11/R13-B/R13-C content validated against the published
  resource identity.
- Script bytecode remains compiled (explicitly excluded as R13-G).
- No broad "map data" exemption.

## 20. Subdivision decision

**Do NOT split R13-F.** The map header→layout→events→connections graph is
tightly coupled (the header carries all 4 pointers; publishing them separately
would require four independent cutovers that all depend on the same header
table). The event bundles are per-map and publish in the same transaction as
the header. The movement cutover is deferred (§8) so it does not add a
sub-stage. Keep F as one stage with the internal subfamily separation in the
seam.

## 21. Highest-risk F family

**The map header → native-world neighborhood rebind.** The map header carries
all four pointer edges (layout → R11, events → F, scripts → G, connections →
F). A wrong pointer reconstruction silently corrupts every map transition,
every NPC event, and every connection. The R11 native world neighborhood
consumes the same map/layout tables — if the seam's published tables differ
from the compiled ones in any consumer-visible way, the world renderer breaks.
**STOP gate:** native-world-neighborhood render proof (R11 Harness B) must
pass with the published tables; every map header's layout pointer must resolve
to the R11-published layout resource; the script pointer must continue to
resolve through the existing GBA-logical bridge; the connection target maps
must all exist. **Runtime test:** publish, transition a few maps, verify the
neighborhood resolves correctly.

---

## Projected totals

| Quantity | Value |
|---|---|
| Map headers | 518 × 28 (GBA) / 40 (native) |
| Map layouts | 441 × 24 / 32 |
| Event bundles | ~518 (object 2,776 + warp 1,313 + coord 375 + bg 720) |
| Connections | ~148 sets |
| F resources (approx) | **~1,625** / ~180,977 B |
| Projected pack | **~20,600 entries** (no cap change) |
| State-v5 | **5,855 / 8,192** (+4 family ranges for the EWRAM gMapHeader copy) |
| Subdivision | **no split** (tightly coupled graph) |
| Highest-risk | map header → native-world neighborhood rebind |
| Movement cutover | **deferred** (R13-B leaf wave, not F) |
| Script boundary | precise GBA-logical bridge preserved until G |
| Implementation complexity | medium-high (large resource count + pointer graph + R11 interaction) |

**STOP — R13-F is a design review; no production code modified, no commit
made, R13-G not started. Exact counts/bytes are pinned from the qualified ROM
during the inventory agents' completion and refined before implementation.**