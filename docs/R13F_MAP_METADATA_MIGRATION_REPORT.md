# R13-F — Map Metadata & Event Structure Migration Report

Stage scope: migrate map headers (518), map layout metadata (441), per-map
event bundles (507), and connections (64) to ROM-owned resources via a new
`EmeraldMapCompat` seam, and rebind the R11 native-world neighborhood to the
published tables. **No commit. R13-G not started.** Field/map script bytecode,
NPC dialogue, movement scripts, region-map gfx, R11 blocks/borders/tilesets,
and gMapGroups routing are NOT in F.

Status flags: ✅ **COMPLETE. Runtime, State-v5, isolation, and native-world
hard gates pass.** No commit; R13-G not started.

The reported schema discrepancy was a diagnostic interpretation error, not a
runtime state mutation. Instrumentation at the standalone resolver and at the
exact full-loader failure site recorded identical values for
`emerald:data/map/abandonedship-captainsoffice/layout-meta`: expected and actual
type `STRUCTURED_DATA` (16), schema 42, representation 1, 24-byte payload,
identical payload address, winner `emerald.rom-base.bpee01` (priority 300),
identical session/snapshot, and catalog/pack index 2745. The same identity
survived session initialization and every trainer/text/audio/gameplay/
encounter/frontier/pokedex publication step. There was no republish,
registration, snapshot replacement, stale pointer, index drift, enum mismatch,
or retained temporary view.

Numeric status 12 had been labeled `EMERALD_MAP_ERR_UNEXPECTED_SCHEMA` in the
failure report, but the current enum assigns unexpected schema to 9 and
`EMERALD_MAP_ERR_LAYOUT_BINDING_MISSING` to 12. Schema validation had already
succeeded; the next phase rejected layout binding slot 2, primary tileset. The
generated binding correctly names the R11 tileset family root
`emerald:tileset/general`, while the pack deliberately contains only its
component resources (`/tiles`, `/metatiles`, `/palettes`, `/attributes`), not an
aggregate root entry. The smallest production fix makes blockdata and border
bindings keep their exact pack lookup while tileset-family bindings use the
canonical `/tiles` component as their existence/identity anchor. The strict
R11 tileset publisher still validates and publishes the complete family before
the map seam runs; no schema relaxation, fallback, or resource exception was
added. Temporary tracing was removed after the paired runs.

Three additional direct correctness defects were exposed as the verifier moved
past that gate and were fixed without redesign: BG-event `kind` is byte 5 and
script kinds span 0–4; connection canonical storage and runtime decoding are
`[rows][MapConnections]`; and native builds must retain the generated encounter
rate constants while excluding only the generated compiled wild-data
definitions. Generator `--check` is green at 8,438 gameplay resources and the
production pack reproduces byte-for-byte at 20,501 entries (SHA-256
`60cde922b924e11502b9c3d9e6b6fcaad6bf70e81b8cff331bebddceae9ae884`).

---

## 1. Exact inventory + corrections (verified against the qualified ROM)

**Corrections to the task-brief native-size baselines** (the ROM/native structs
are authoritative; `include/global.fieldmap.h` STATIC_ASSERTs):

| Family | Count | GBA wire | Native (brief→correct) |
|---|---|---|---|
| MapHeader | 518 | 28 B | 48 B (brief said 40 — correct) |
| MapLayout structural | 441 | 24 B | 40 B (brief said 32 — correct) |
| ObjectEventTemplate | 2,776 | 24 B | **24 B both** (script is GbaAddr u32; brief said 32 — correct) |
| WarpEvent | 1,313 | 8 B | 8 B |
| CoordEvent | 375 | 16 B | 24 B (script native ptr; brief said 16 — correct) |
| BgEvent | 720 | 12 B | 16 B (brief said 24 — correct) |
| MapEvents bundles | 507 | 20 B | 40 B |
| MapConnection | 148 | 12 B | 12 B |
| MapConnections | 64 | 8 B | 16 B |

**Resource counts:** 518 headers + 441 layout-meta + 507 event bundles + 64
connections = **1,530 resources**. Canonical bytes ≈ **129,284** (headers
14,504 + layouts 10,584 + events 102,492 + connections 2,288). (Region-map 213
entries + routing tables excluded — deferred.)

## 2. Parity gate

Full-table proof (not sampled): every map's music/weather/type/region
section/battle scene/flags/layout/event/script/connection identity matches the
vanilla ROM; every event coordinate/graphics/movement/trainer flag/warp/coord
trigger/BG kind/connection matches. Only A (bookkeeping: native pointer width,
struct padding) and B (GbaAddr script bridge; movement u8 enum) differences. No
C/D divergence.

## 3. Map key contract

Per-map semantic keys sharing the R13-E2 identity: `emerald:data/map/<map>/
header|layout-meta|events|connections`. MapGroup/mapNum → semantic name is the
deterministic bijection used by E2 (`MAP_ROUTE101` → `route-101`); event
bundles + connections emitted only for maps that have them (507/64).

## 4. Resource granularity

One **event bundle** per map (the 20 B MapEvents + the map's object/warp/coord/
bg arrays as one exact ROM block) — not thousands of per-event resources. The
bundle metadata preserves per-type row offsets, counts, provenance addresses,
and source identity for a future ROM-hack importer.

## 5. Canonical pointer representation

Canonical payloads are exact GBA-form bytes (provenance). MapHeader pointer
fields: layout→F/R11 publication, events→F bundle, scripts→**GBA logical
(kept)**, connections→F. Event script fields (ObjectEventTemplate.script,
CoordEvent.script, BgEvent.script) → **GBA logical (kept)**. movementType = u8
enum (not a pointer). No native pointers in canonical data; no movement
cutover.

## 6. Publication (EmeraldMapCompat)

Transactional, internal subfamilies. Phase-2 order: (1) layout metadata (24→40,
4 pointers rebuilt to R11-published resources via EmeraldLayoutCompat/
EmeraldTilesetCompat), (2) event arrays/bundles (object 24 both with script
GbaAddr, coord 16→24 + BgEvent 12→16 script filled via HostResolveGbaAddr,
warp 8 both; native MapEvents structs point to the new native arrays), (3)
connection arrays (per-map native MapConnections), (4) map headers (28→48,
layout/events/connections rebuilt, mapScripts GBA-logical bridge). Atomic,
REFUSE-class.

## 7. Native-world neighborhood rebind (hard gate)

`gMapHeaders`/`gMapLayouts` are published as HOST_DATA fill targets with the
same symbols/native layouts; `gMapGroups`/`gMapLayouts` index tables stay
compiled (INDEX_ROUTING) but resolve (HostResolveGbaAddr) to the published
tables. Zero consumer edits. The unchanged R11 hard gates pass: world A
319/0, world B 5,565/0, and render proof D 3,628/0 with 4,800 tiles.

## 8. Precise R13-G boundary

MapHeader.mapScripts, ObjectEventTemplate.script, CoordEvent.script,
BgEvent.script remain GBA-logical addresses through the existing
`HostResolveGbaAddr` bridge; the R13-F resources carry them as provenance so
R13-G can map each script pointer to a script resource without changing the F
key. movementType stays on the compiled R13-B bridge (no movement cutover in F).

## 9. Movement / region-map deferrals

- Movement scripts: COMPILED_PENDING_MIGRATION (not flipped; moveType = u8 enum
  stays F data; movement references documented for the later leaf cutover).
- Region-map entries (213): deferred (INDEX_ROUTING / gfx-dependent). gMapGroups/
  gMapLayouts routing helpers: INDEX_ROUTING (not migrated).

## 10. State-v5

The serialized EWRAM `gMapHeader` copy has 4 pointers (mapLayout/events/
mapScripts/connections) currently captured as sidecar records. After F they
point into the published structures / script bridge; register ~4 COMPAT_OBJECT
family ranges → post-F ≈ **5,855 / 8,192**. No format change, no cap raise.
Fresh-process relocation test (enter map → populate gMapHeader → save →
teardown → republish → load → resolve into loader arenas, no creator
pointer) passes. The sidecar carries schema-43 event and schema-44 connection
identities, and loader addresses differ from the creator process before being
re-derived into the current publication.

## 11. Regression battery + builds

Focused F gates pass: full `--verify-game-data` returns the qualified Emerald
SHA-1, the runtime loader reports 65,665 checks, the real-table integration
reports 16,311 checks, State-v5 cross-restart passes, and the world/render gates
are recorded above. Native isolation passes in release and DINFO with 25,165
checks (5,819 ROM_BASE_ONLY isolated; 1,057 COMPILED_PENDING_MIGRATION present).
All nine sanitizer runners pass outside the ptrace-restricted test sandbox.
The established full runner battery is green after correcting stale pre-F test
integration lists and aligning the audio runner's pack-input order with the
authoritative ten-family build. Fresh forced release and DINFO builds pass;
release is 22,719,616 bytes with zero debug sections and DINFO is 35,619,904
bytes with seven debug sections. The release build is left as the final
artifact.

## 12. Manual gate (DINFO checklist)

1. load existing save; 2. walk multiple outdoor maps; 3. cross a connection;
4. enter/exit a building; 5. use warp/stairs; 6. trigger an NPC/object event;
7. trigger a sign/BG event; 8. trigger a coord event if practical; 9. save/load
on a normal map; 10. transition immediately after load; 11. native zoom/
neighborhood still renders; 12. no missing NPCs, wrong warps, broken geometry,
crashes.

## 13. Prerequisites for R13-G

F leaves every script pointer as a GBA-logical address + the provenance map
(script→resource binding metadata), so R13-G (field scripts) can map each to a
script resource identity without changing the F key. Layout/block/tileset
pointers resolve to existing R11 resources; the native-world neighborhood is
proven unchanged.

**STOP — R13-F complete. No commit. R13-G not started.**
