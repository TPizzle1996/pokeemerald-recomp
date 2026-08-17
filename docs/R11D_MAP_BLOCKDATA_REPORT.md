# R11-D Report — Raw Map Blockdata Migration

## 1. Inventory results/counts

`tools/gen3_resources/layout_family/gen_layout_inventory.py` reads
`data/layouts/layouts.json` (441 `struct MapLayout` records) and scans the
882 payload `.bin` files (`map.bin` blockdata + `border.bin` border words).
Deterministic output + `--check` mode; hard pins (fail on deviation): 441
layouts, 441 blockdata leaves, 441 border leaves, 32 distinct border byte
patterns (the R11-A audit's "4 u16 uniform" claim is wrong — verified during
R11-D exploration), canonical slug uniqueness (two leaves per layout share
the layout slug), orphan-file detection.

- 441 blockdata leaves (`map.bin`), payload = file bytes verbatim (the GBA
  build INCBINs the file); sizes w×h×2 each — largest 6400 blocks
  (Underwater_Route127 80×80), smallest 1 block
- 441 border leaves (`border.bin`), 8 B each (4 u16, 2×2)
- **32 distinct border hashes** (116× `9ff63640…`, 105× `efef8d5a…`,
  35× `0c2871e5…`, 26× `a8fb987b…`, …) — recorded; **no dedup** (441
  resources, one per layout: the R9 alias machinery is for mapping data, not
  pack byte savings)
- **20 deviating layouts** (sizes differ from w×h×2): 18 unused 1×1 layouts
  → 4 B, CaveOfOrigin_Unused_B4F_Lava 724 vs 722,
  LittlerootTown_ProfessorBirchsLabWithTable 340 vs 338
- **882 resource leaves**, 0 `[[gba_parity]]` rows — blockdata/border are
  raw (uncompressed) on both targets, no LZ split

## 2. Generated resource IDs/counts

Canonical = lowercased layout name minus the `_Layout` suffix (`<slug>`),
matching the R11-C tileset id convention:

- `emerald:layout/<slug>/blockdata` (type `tilemap`, schema 1, raw,
  gba-tilemap, w×h×2 bytes) — 441
- `emerald:layout/<slug>/border` (type `tilemap`, schema 1, raw,
  gba-tilemap, 8 bytes) — 441

**882 resources**, bytewise-sorted, duplicate-canonical detection.
`GEN3_RESOURCE_TYPE_TILEMAP` already existed in the runtime vocabulary
(resource_types.h:16) — no type-system change.

## 3. Generator files

- `tools/gen3_resources/layout_family/gen_layout_inventory.py` —
  inventory extraction (deterministic, `--check`; the 32-distinct-border
  pin lives here)
- `tools/gen3_resources/layout_family/gen_layout_family.c` + Makefile →
  `gen-layout-family` binary — validates every artifact (raw byte identity,
  blockdata length == w×h×2 except the 20 documented deviations, border
  length == 8, slug uniqueness, tileset references against the R11-C
  `tileset_native.generated.h` externs), emits
  catalog/bindings/ownership/consumers + two seam-side generated headers:
  - `include/emerald/resources/layout_native.generated.h` (3551 lines) —
    all 441 `struct MapLayout` records as writable C objects via
    `LAYOUT_NATIVE_DEFINE` (`.width/.height/.border/.map` +
    `.primaryTileset/.secondaryTileset` pointing at the R11-C tileset
    records); every other includer gets extern declarations
  - `include/emerald/resources/layout_frames.generated.h` (457 lines) —
    publication macro rows: symbol + blockdata/border resource ids + sizes
- Build isolation per the R7A §24 precedent: `-std=c99 -Wall -Wextra
  -Werror`, no Emerald defines; links the exact M0/M1 key-derivation code.

## 4. Generated manifests

- `resources/extraction/emerald/bpee01/layout/inventory.generated.toml`
  (882 records, `raw` encoding, ownership_state `ROM_BASE_ONLY` for native /
  `COMPILED` for gba per `[resources.targets]`)
- `.../catalog.generated.toml`, `bindings.generated.toml`,
  `ownership.generated.toml` (882 resources),
  `layout_consumers.generated.toml` (441 layout records with tileset refs)
- `.../manifest.production.toml` — 882 `[[records]]` via gen3-elf-manifest
  (provenance: qualified pret reference ELF d8e405c4f6b48f1faf3b26a3e045f0df2ff3ecb7
  + upstream symbol rename b89722500, retail-matching recomp ROM; BPEE01
  Rev 0, ROM SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7),
  deterministic (`--check` passes, no-op after regeneration)
- elf_manifest vocabulary addition (checkpoint 4):
  `gba-tilemap` canonical representation = raw little-endian u16 entries
  (`manifest.c:145-148`); validation vocabulary, no format change.

## 5. Exact production pack rebuild path/command

```
tools/gen3_resources/pack_build/gen3-pack-build \
  --rom ../pokeemerald-reference/pokeemerald.gba \
  --output games/emerald/base/emerald-bpee01-v1.rpack \
  --manifest resources/extraction/emerald/bpee01/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/object_event/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/tileset/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/layout/manifest.production.toml \
  --catalog resources/catalogs/emerald/catalog.toml \
  --catalog resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml \
  --catalog resources/extraction/emerald/bpee01/object_event/catalog.generated.toml \
  --catalog resources/extraction/emerald/bpee01/tileset/catalog.generated.toml \
  --catalog resources/extraction/emerald/bpee01/layout/catalog.generated.toml
```
(`--check` verifies byte-identical determinism.)

## 6. Pack old/new resource counts and hashes

- Old (R11-C): 3636 entries, 5,220,944 bytes, sha1 14eb935e3c341bca3a337115b5799ae505d8363c
- New: **4518 entries** (196 trainer + 1608 pokemon + 288 object-event +
  1544 tileset + 882 layout), 6,059,040 bytes, sha1
  **99ea64e90eff67c33204ea10a0d55086d6dc567f**
- Prior entries unchanged by construction (same inputs + deterministic
  writer); deterministic rebuild verified byte-identical; production proof
  (37921 checks) green against the new pack.
- Import cap 4096→8192 (`EMERALD_IMPORT_MAX_RECORDS`, emerald_resource_import.c:58;
  merged manifest/catalog views hold 4518 records); range-index cap
  `EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES` 4096→8192
  (emerald_resource_ranges.h:56; 4518 merged ranges). `MAX_PAYLOAD` 16 MiB
  unchanged (largest blockdata 12.8 KB).

## 7. Compat seam implementation

`src/emerald/resources/emerald_layout_compat.c` (+ header), 354 lines:
`#define LAYOUT_NATIVE_DEFINE` + `#include
"emerald/resources/layout_native.generated.h"` defines all 441 records as
writable C objects. `BuildEntries()` builds 882 rows (per layout: blockdata
id+size, border id+size) from the generated frames header.
`TryInitialize` (transactional, fail-closed): verifies all 882 entries
present in the snapshot, `payloadSize == expectedSize`, `winningProviderId
== EMERALD_ROM_BASE_PROVIDER_ID`; publishes only on success (nothing
mutated on failure). `PublishFromImage()`: per layout, `record.map =
FindStream(blockdataId)`, `record.border = FindStream(borderId)` — pointers
into session memory (same as R11-C tiles/metatiles/attrs).
`ClearMigratedEntries()`: `record.map = NULL; record.border = NULL`.
`Republish()` allocation-free idempotent; `Shutdown()`; `GetImage()`;
`GetEntryCount/GetEntrySchema/GetEntryRole` — all entries canonical
(`EMERALD_RESOURCE_ROLE_CANONICAL`, no LZ rows). Guarded
`#if PLATFORM_SDL2 && NATIVE_LINUX`; the GBA build never compiles it.

## 8. Record publication design (publish-in-place)

The layout records are the compiled objects — no clones. On native,
`layouts_table.inc` stays untouched: its `gba_ptr` entries are four-byte
symbolic references that resolve to the seam-TU record symbols (non-PIE,
< 4 GiB — `HostResolveGbaAddr` contract, host_memory.c:325). mapjson's
`generate_layout_headers_text` (mapjson.cpp:606-607) wraps the payload
`.incbin`s AND the record definitions in `.if LINUX64` … `.else` … `.endif`:
GBA build emits the old bytes verbatim; native emits a comment only. The
payload symbols have no direct C references (verified: no
`*_Blockdata`/`*_Border` symbol use in `src/` — all access flows through
`mapLayout->map`/`->border`, fieldmap.c:54-59 macros,
`MapGridGetMetatileIdAt/BehaviorAt/LayerTypeAt`, region_map.c, overworld.c),
so no zero-filled shells are needed. **No game-source changes at all**:
`GetMapLayout()` (overworld.c:549), battle_pyramid.c:1540, and every
`gMapHeader.mapLayout->map/border` consumer keep reading the resolved
record; the record *is* the writable seam object.

## 9. Lifecycle wiring (umbrella driver)

`emerald_trainer_native_compat.c` (the umbrella driver):

- Strict path (after the R11-C tileset block,
  `TryInitialize` at :896-906): layout family not published → init error
  ("R11-D compiled leaves are gone") — blockdata absence makes maps
  unrenderable.
- Additive unit-harness path (:834-842): missing family → NULL-sentinel
  warning, status OK (mirrors the R11-C opt-in block).
- `Republish` (:588), `ClearMigratedEntries` (:641), and the
  image/schema/role registration (:1037-1050) alongside the tileset family.

## 10. ROLE_CANONICAL registration proof

All 882 streams registered under EMERALD_RESOURCE_ROLE_CANONICAL
(emerald_layout_compat.c `GetEntryRole`); any registration failure fails
closed. Rationale as in R11-C: the bytes ARE the canonical raw payloads;
blockdata/border have no compressed representation at all (no LZ split).

## 11. State v5 integration

No State v5 format change. The layout image is session-scoped: it is
rebuilt from the snapshot at restore (TryInitialize → RebuildRangeIndex
flow), so record pointers re-derive post-load exactly like the tileset
family. The cross-restart harness proves it (below): `mapLayout->map` /
`->border` point into the new session arena after load. Future
serialized-slice pointers into these streams would be captured as
identity+offset with role CANONICAL by the R10 walker (ranges registered).

## 12. Cross-restart proof

The R10 cross-restart harness (real walker + seams + production pack)
publishes the layout family in both processes; the loader process
re-derives the four pinned record pointers to its own fresh arena:

```
layout arena: captains=0x55f9c57640e8 petalburg=0x55f9c5797bc0 route127=0x55f9c57f8050 unusedoutdoor=0x55f9c57fffc0
layout arena: captains=0x55cf9e58b0e8 petalburg=0x55cf9e5bebc0 route127=0x55cf9e61f050 unusedoutdoor=0x55cf9e626fc0
```

Different heap bases across processes, identical relative layout
(`captains` … `unusedoutdoor` keep their arena-relative offsets) — no stale
pointers survive a state load. Cross-restart suite: ALL PASSED.

## 13. Isolation proof

Native link loses all 882 payload leaves. Isolation runner (driven by the
6-file/4518-record union of all ownership files: base + pokemon_battle +
object_event + tileset + **layout**) proves it three ways:

(a) payload byte scans over the binary — 0 unexpected hits after the 10
sha-keyed exemption groups: 6 tiny blockdata patterns and 4 8-byte
repeating border patterns, all classified at scan time as x86 instruction
coincidences (e.g. `0102` = ADD [rdx],eax ×4728 in .text; `0c06` = OR
AL,0x06 ×392; the 4-byte `27bfae0e…`/`18ff0eb8…`/`e0b47af9…`/`95bbb690…`
groups coincide with pre-relocation pointer addends in maps.o's own gba_ptr
table) or as legitimate border bytes that also appear in instruction runs
(`efef8d5a…` 0100-repeating border ×960, `9ff63640…` 0102 border —
SetOpponentMonData etc., `0bde8b5d…` 0802 ×53, `eaf2d699…` 1002 ×35).
25 tiny blockdata records < 32 B total; **zero payloads > 8 bytes anywhere;
0 `_Layout_Blockdata` / `_Layout_Border` symbols in the binary
(nm-verified)**.

(b) per-record object scan: every `emerald:layout/*` record's encoded bytes
are searched in the native `maps.o` — zero hits (the `.if LINUX64` skip
means maps.o carries no layout payload); the preprocessed-TU check does not
cover `layouts.inc` (it is ASM, not a C TU), so this object scan is the
hard isolation proof for the ASM path.

(c) native dependency-graph check via the build's own preprocessed TUs
(pre-existing guardrail).

**19953 ok, 0 failed; PASS 4518/4518 ROM_BASE_ONLY isolated (196 + 1608 +
288 + 1544 + 882), 0 COMPILED_PENDING_MIGRATION.** GBA build unchanged.

## 14. Tests + sanitizer counts

- layout compat test (`tests/emerald_layout_compat_test.c` + runner):
  real seam + generated tables + unit-harness stubs — transactional
  failure modes, Republish idempotence, ClearMigratedEntries, ROLE_CANONICAL,
  size-mismatch diagnostics — **ALL PASSED (441 layouts, 882 entries)**.
- cross-restart suite: all green incl. the new layout arena checks.
- production proof: 37921 checks green on the 4518-entry pack.
- full battery (all 18 run_*.sh scripts, exit 0): resource_lz 144 ·
  resource_import 155 · rom_base_provider 140 · trainer_native_compat 8377 ·
  runtime_loader 2013 · real_tables 16311 · trainer production 37921 ·
  native asset isolation 19953 · ranges · fingerprint · cross-restart
  state · object-event compat · tileset compat (75 structs / 1544 entries)
  · layout compat (441 layouts / 882 entries).
- sanitizer variants: resource_import_sanitize 155, resource_lz_sanitize
  144, rom_base_provider_sanitize 140, trainer_native_compat_sanitize 8377
  — all exit 0.
- `git diff --check` clean.

## 15. Files changed

New: tools/gen3_resources/layout_family/* (gen_layout_inventory.py +
gen_layout_family.c + Makefile), src/emerald/resources/emerald_layout_compat.c,
include/emerald/resources/emerald_layout_compat.h,
include/emerald/resources/layout_native.generated.h +
layout_frames.generated.h, resources/extraction/emerald/bpee01/layout/*
(6 generated manifests), tests/emerald_layout_compat_test.c,
tests/run_emerald_layout_compat.sh, docs/R11D_MAP_BLOCKDATA_PLAN.md.
Modified: tools/mapjson/mapjson.cpp (LINUX64 skip in
generate_layout_headers_text), elf_manifest (tilemap/gba-tilemap
vocabulary row), src/emerald/resources/emerald_resource_import.c (8192
cap), include/emerald/resources/emerald_resource_ranges.h (8192 range
cap), src/emerald/resources/emerald_trainer_native_compat.c (lifecycle
wiring), tests/run_emerald_native_asset_isolation.sh (6-file union +
layout exemption groups + per-record maps.o scan),
tests/run_emerald_trainer_native_compat_production.c/.sh (4518 checks +
layout catalog), tests/emerald_resource_import_test.c (cap tests
8193-record manifest + 2×4097 merged), 5 runners (link lists gain
emerald_layout_compat.c), 2 guardrail runners (--exclude
emerald_layout_compat.c).

## 16. Warnings/issues

- 10 sha-keyed byte exemption groups (section 13) are coincidental byte
  identity with x86 instruction streams and pre-relocation pointer addends
  in maps.o's own gba_ptr table — documented per-sha with reasons in the
  runner; classified as REPORTED/NOTE not FAILED (R11-C §7 precedent); the
  symbol/TU/object-scan checks are the hard isolation proof.
- 20 deviating layout sizes (18 unused 1×1 → 4 B, CaveOfOrigin_Unused_B4F_Lava
  724 vs 722, LittlerootTown_ProfessorBirchsLabWithTable 340 vs 338) —
  documented in the inventory; payload = file bytes verbatim, no fix needed.
- 32 distinct border patterns but **no dedup** — 441 border resources
  (3.5 KB in the pack) per the R11-D plan decision; the fact is recorded
  in the inventory and report.
- Build-system quirk (from R11-C, applies here): `Makefile_pc` line 181
  `.SECONDARY:` marks all files secondary, so fresh native builds no-op
  unless forced with `-B`. All rebuilds in this report used `-B`.

## 17. DINFO build status

Both binaries rebuilt from the final tree with `-B`:

- `pokeemerald-linux64` (release): 30,246,512 bytes, 0 debug sections,
  Build ID ea669d3f09108af692f24ad7c6f36866b348f95b, sha1
  6652f6bf940c066ba089b6990be26db1ea60c3e2 — the `-B` rebuild reproduced
  the checkpoint-10 release byte-for-byte (deterministic), 8
  `EmeraldLayoutCompat` symbols nm-verified; isolation re-verified green
  (19953 ok, 0 failed) against it.
- `pokeemerald-linux64-dinfo` (DINFO=1): 42,478,576 bytes, 7 debug
  sections, `-g -O0 -rdynamic`, Build ID a7d13fc8bcfcc76657d73fc8fa92259ca4fab73b,
  sha1 b1202ed816b0139a47752334bf228114c97f080e.

Build IDs differ as expected; `git diff --check` clean.

## 18. Concise manual validation checklist (release gate)

DINFO build first (section 17); then with the release binary:

1. Littleroot Town overworld — map blocks render from published blockdata
2. Walk around Littleroot — collisions solid (walls/trees block; no
   pass-through; tall grass walkable)
3. Petalburg City (30×30) — full layout renders, walkable
4. Route 101 north connection — blocks correct across the connection
5. Map-edge border rendering — the layout's border words render around the
   map (not garbage/black)
6. An interior (e.g. the player's house) — blockdata + border correct
7. A cave/underwater map (e.g. Dewford Cave / Route 127 underwater, the
   80×80 largest) — renders fully, no artifacts
8. Warps/doors — entering a building transitions to the correct blockdata
9. Several map transitions (route→town→interior→route) — no stale blocks
10. Save state in the overworld → quit completely → relaunch → load → map
    identical (no garbage, no stale blocks, collision still correct)
11. Repeat once
12. Visuals pixel-identical to pre-R11 behavior
13. Collision behaviors match pre-R11 (surf on water, no walking into
    walls, ledges/stairs behave)
14. DINFO build also boots to the overworld without errors

## 19. R11-D addition — deviating layout sizes

20 layouts deviate from `w×h×2` (section 1): 18 unused 1×1 layouts carry
4-byte blockdata (2×2 blocks), CaveOfOrigin_Unused_B4F_Lava and
LittlerootTown_ProfessorBirchsLabWithTable differ by ±2 from the formula.
The inventory records the actual lengths; the generator validates against
the recorded sizes, not the formula; the seam's `payloadSize ==
expectedSize` check uses the recorded sizes.

## 20. R11-D addition — border storage decision

32 distinct border byte patterns across 441 layouts, but the pack stores
441 border resources (no dedup). Rationale: the R9 alias machinery exists
for mapping data (address/ownership semantics), not pack byte savings;
3.5 KB in the pack is below the R7A pack-size noise floor. The
32-distinct fact is pinned in the inventory (regeneration fails on
deviation) and documented here.

## 21. R11-D addition — isolation exemption classification

All 10 sha-keyed groups carry explicit one-line reasons in the runner
(`layout_byte_exemptions` dict) and were classified at scan time, not
silently whitelisted: 2-byte blockdata patterns = x86 instruction opcodes
(ADD [rdx],eax 4728 hits; OR AL,0x06 392 hits); 4-byte patterns =
pre-relocation pointer addends inside maps.o's own gba_ptr table (e.g.
offset 0x0606) plus x86 runs; 8-byte borders = legitimate border bytes that
coincide with instruction runs (0100-repeating ×960; SetOpponentMonData
etc.; ×53; ×35). Verified via readelf/nm section+symbol attribution before
classification; zero payloads > 8 bytes exist anywhere in the binary.

## 22. R11-D addition — maps.o object scan (ASM-skip proof)

`layouts.inc` is ASM, so the preprocessed-TU check cannot cover the
`.if LINUX64` skip. The runner therefore scans the native `maps.o` object
for every layout record's encoded bytes (layout records only; the object
is built from the same tree as the binary). Zero hits = the skip emitted no
payload bytes into the GBA-path object's native twin; symbol check adds
`_Layout_Blockdata`/`_Layout_Border` absence (nm, 0 symbols).

## 23. R11-D addition — generator determinism

`gen-layout-family --check` re-derives all 6 outputs from the inventory
and compares bytewise (deterministic no-op when unchanged); inventory
regeneration is a no-op diff (pins: 441/441/441/32). Verified after the
checkpoint-3 mapjson regeneration: the LINUX64=0 (GBA-path) layouts.inc
output is byte-identical to the pre-change file.

## 24. Expanded manual checklist (superset of section 18)

The section 18 list is the release gate. Add, when practical:
15. Underwater Route 127 (80×80, largest blockdata) — scroll far corners,
    camera never shows garbage blocks
16. A map entered directly via warp on load (no walk-in) — blockdata
    correct on first frame
17. Battle Pyramid (battle_pyramid.c:1540 path) floor layout — accessor
    returns correct blockdata
18. A border-tile-dependent map edge (e.g. a cave interior where the
    surrounding border is fully visible) — border words correct on all 4
    sides
19. Map change mid-animation then save state — load after relaunch shows
    the new map, not the old
