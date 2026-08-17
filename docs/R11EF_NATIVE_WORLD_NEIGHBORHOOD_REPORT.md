# R11-E/F: NativeWorldNeighborhood + connected-map composition — report

Plan: `docs/R11EF_NATIVE_WORLD_NEIGHBORHOOD_PLAN.md`. This report records the
checkpoint-by-checkpoint evidence, the three documented corrections to the
plan (link set, Harness D rect dimensions, isolation exemptions), the full
battery result, the build status, and the §14 manual validation checklist.
**E/F is complete through checkpoint 9 and STOPS here for manual validation.**

## 1. What E/F delivered

- `src/platform/native_world_neighborhood.c` +
  `include/platform/native_world_neighborhood.h` — the native world
  neighborhood module: resident-map builder (`EnsureCurrent`), world-block
  coordinate formulas, precedence + border parity, `ResolveBlockForRender`,
  `MetatileTileEntryForLayer`, object-event accessor, fail-closed DEGRADED
  path (legacy RING/GRID owns every query when residency cannot be
  established).
- Two guarded lifecycle hook insertions in `src/platform` files only
  (`desktop_game_content.c` Init; `native_state.c` Invalidate) — neither
  compiled by the GBA build.
- Harnesses A/B/C/D (below).
- **No gameplay behavior change, no zoom, no recursive second-hop
  residency, no second resource system, no pack-format change, no State v5
  format change.** The module stores nothing in any serialized slice: its
  storage is host .data/.bss, outside BuildSlices' game-owned ranges and the
  gba sections (plan §10).

## 2. Checkpoint evidence

| CP | Plan requirement | Result |
|---|---|---|
| 1-2 | Header + module; native build links; GBA untouched | Module compiled into both native binaries (5 of 6 query symbols nm-verified in each); `git diff --name-only` outside src/platform/tests/docs empty; `git diff --check` clean |
| 3 | Harness A green (synthetic geometry) | **319 checks, 0 failures** (plain + sanitize) |
| 4 | Harness B green (real data + production pack) | **5551 checks, 0 failures** (plain + sanitize) |
| 5 | Lifecycle hooks; release + DINFO rebuild; state suite green | Both binaries rebuilt with `-B` (see §9); state tests green (format untouched) |
| 6 | Harness C green (state proofs) | Cross-restart resource state tests: **ALL PASSED** (plain + sanitize) — R11-E complete |
| 7 | Harness D green (offscreen render proof) | **3628 checks, 0 failures, 4800 tile entries** (plain + sanitize); presentation-path diff empty (see §6); zoom off — R11-F complete |
| 8 | Full battery + isolation unchanged | **30/30 PASS, BATTERY FAIL=0**; isolation **19953 ok / 0 failed, 4518/4518** (see §7) |
| 9 | `git diff --check`; readelf checks; distinct Build IDs; this report | `git diff --check` clean; see §9; **STOP for manual validation** |

## 3. Harness D — the renderer-facing proof (R11-F)

`tests/emerald_native_world_render_proof.c` + `run_emerald_native_world_render_proof.sh`.
Offscreen tilemap-entry render — **no pixels, no SDL, no viewport code** —
over the real Littleroot ↔ Route101 north connection (Littleroot group 0
num 9; Route101 group 0 num 16, NORTH offset 0; both 20×20 maps; connection
pins Littleroot at world (0,0), Route101 at (0,−20)):

- Session: pack + 5 family catalogs → `BuildRomBaseCandidate` (4518 entries,
  provider `emerald.rom-base.bpee01` precedence 300) → snapshot →
  `EmeraldResourceCompat_InitializeFromSnapshot` (publishes the R11-C
  tilesets + R11-D layouts) → `NativeWorldNeighborhood_Init` → set location
  Littleroot.
- For every block of the rect, `ResolveBlockForRender` + 4 quadrant entries
  per BG written into three 20×20×4 tilemap buffers (4800 entries), then
  byte-compared against an **independent oracle** that reimplements the
  resolution directly from the two published layouts' `.map` + metatile
  tables (the two-independent-derivations pattern).
- Assertions (plan §11): (i) rect exceeds 240×160 — holds in **both**
  dimensions (320 > 240 and 320 > 160); (ii) rows y < 0 come from Route101's
  blockdata with Route101's tilesets, rows y ≥ 0 from Littleroot's; (iii)
  oracle byte-match; (iv) source enum flips exactly at y = 0 and stays
  BORDER nowhere inside the rect (row sources: row 0 = NEIGHBOR, row 19 =
  CURRENT, flip count 1); (v) presentation path untouched (§6).
- BORDER control cells at (−5, 5) and (0, −30) resolve BORDER with
  mg/mn = Littleroot, as required.
- Metatile byte rule verified from `field_camera.c` `DrawMetatile`
  (lines 257-303): SPLIT → BG3 = tiles[quadrant], BG1 = tiles[quadrant+4],
  BG2 = 0; COVERED → BG3 = tiles[quadrant], BG2 = tiles[quadrant+4], BG1 =
  0; NORMAL → BG3 = 0x3014, BG2 = tiles[quadrant], BG1 = tiles[quadrant+4].

### 3.1 Correction — Harness D rect dimensions (MUST note)

The plan §11 parenthetical "480×160 px at 8 px/tile, 60×20 tiles" is
**internally inconsistent** with its own rect, `x ∈ [0,20), y ∈ [−10,10)`:
20 blocks × 16 px/block = **320×320 px**, not 480×160. The rect as stated
is what was implemented (20×20 blocks, 400 cells, 4800 tile entries), and
it satisfies every assertion of the plan: (i) in both dimensions
(320 > 240 and 320 > 160), and (ii)/(iv) which force rows on both sides of
y = 0 (only possible with height 320 — a 480×160 rect 2 tiles tall cannot
span the connection). The parenthetical dimensions are wrong; the rect +
assertions are authoritative.

## 4. Correction — Harness B/D link set (MUST note)

The plan §10's "mutually self-contained closure" (`maps.o` + `map_events.o`
+ `event_scripts.o`) **does not hold**, discovered and corrected during
CP4 (full audit in the runner header, `tests/run_emerald_native_world_real.sh`):

- `maps.o` is clean apart from the R11-D `<Map>_Layout` seam and 518+518
  `<Map>_MapEvents`/`<Map>_MapScripts` relocations.
- `map_events.o` carries **2260 undefined refs** into event scripts AND game
  functions; `event_scripts.o` carries **769** (`gScriptCmdTable` + script
  bytecode). Neither is self-contained.
- The module never dereferences `mapScripts` or the events-record fields
  (it reads only `mapLayout` + `connections`; events only for
  `objectEventCount` in `GetObjectEventsAt`, which no Harness B test pins
  to a real value).
- Resolution: a **generated stub object** (`.quad 0` pointer targets,
  generated from `nm -u maps.o` — the same source that names the relocation
  targets) replaces `map_events.o`/`event_scripts.o`, plus the
  `emerald_native_world_tables_stub.c` native-table stubs and the Overworld
  accessor stub. Real headers, layouts, connections, blockdata and tilesets
  stay canonical; the script/event pointers are never read.
- Harness D uses the same corrected link set.

## 5. Harness A/B/C summary

- **Harness A** (`emerald_native_world_neighborhood_test.c`, hand-built
  fixtures over a stub `Overworld_GetMapHeaderByGroupAndId` + fixture
  `gMapGroups`): 319 checks — N/S math, E/W math, positive/negative
  offsets, asymmetric dims, multiple neighbors, duplicates + cycles,
  outside-union BORDER parity incl. IMPASSABLE, current precedence with an
  adversarial overlap.
- **Harness B** (`emerald_native_world_real_test.c`, real maps.o + real
  pack): 5551 checks — Littleroot↔Route101 both directions (origins
  (0,−20)/(0,+20), Oldale), no-connection interior (MaysHouse_1F,
  neighborCount 0), queries inside current + neighbor, identity mutation →
  version bump + rebuild, no second hop (Route101 residents {Route101,
  Oldale, Littleroot} only), Route124 4-spatial-neighbors (no
  Underwater_Route124), data-wide pin sweep (148 total = 134 spatial + 7
  dive + 7 emerge; 61 maps with ≥1 spatial connection, 3 dive/emerge-only;
  max 4 spatial neighbors; max 2 same-direction; 0 back-connection
  violations; 18 negative offsets; duplicate pairs' rects non-overlapping),
  synthetic-map fail-closed (pyramid → DEGRADED, synthetic blocks, version
  unchanged, GetBlock/ResolveBlockForRender FALSE).
- **Harness C** (state proofs): corrupt records → save + load → fresh
  rebuild from identity, garbage gone, fresh pointers; not-serialized
  proven at three layers (section attribution, range non-intersection,
  behavioral).

## 6. Presentation path gate (assertion v)

`native_overworld_renderer.c`, `native_overworld_viewport.c`,
`native_field_compositor.c`, `field_camera.c` have **ZERO E/F diff**; the
E/F change set (`native_world_neighborhood.{h,c}` + tests + docs) contains
no presentation-path edits. For the record, the working tree's only
presentation-adjacent deltas are pre-existing branch content, not E/F:
`desktop_video.c` (R11-A..D renderer era) and the 12-line PORTABLE-only
`GetCameraTileOffsets` accessor in `field_camera.c` (added for the native
renderer snapshot capture, predating E/F; excluded on GBA by `#ifdef
PORTABLE` so the ROM layout stays retail-identical).

## 7. Battery + isolation

Full battery (all `tests/run_*.sh` plain + `[sanitize]`-mode runners, run
from the repo root — the documented requirement for
`run_emerald_resource_import_sanitize.sh`, which loads repo-relative
artifact paths):

- **30/30 PASS, BATTERY FAIL=0.**
- Harness A 319, Harness B 5551, Harness D 3628 (all plain + sanitize);
  state tests ALL PASSED; trainer compat 8377×2; real tables 16311; runtime
  loader 2013; rom base provider 140×2; resource lz 144×2; resource import
  155; layout compat 441/882×2; object-event compat 253 sheets / 35
  palettes / 1788 frames×2; tileset compat 75 structs / 1544 entries×2;
  session fingerprint ×2.
- Isolation unchanged: **native asset-isolation: 19953 ok, 0 failed**;
  **PASS: native target matches ownership (4518/4518 ROM_BASE_ONLY
  isolated — 196 trainer + 1608 pokemon battle + 288 object-event + 1544
  tileset + 882 layout, 0 COMPILED_PENDING_MIGRATION)**. Re-verified
  against the final release binary after the §9 rebuilds.

### 7.1 Correction — two isolation byte exemptions (MUST note)

Two pre-existing records fail the encoded-payload presence check by
coincidence-of-compilation in the **DINFO / -O0-class** binary — both
provably **not E/F** (E/F touches no data/build/renderer files; both hit
TUs compile into every native build from this tree; R11D-era scans ran
against a stale binary per the Makefile_pc `.SECONDARY:` no-op trap).
Classified per the runner's documented R7B/R11-C §7/R11-D §10 exemption
mechanism; both hard checks for both records (session-published native slot
present with payload bytes verified absent; symbol/TU/dep-graph) pass:

- `emerald:object-event/npc1/palette` — sha
  `6523ac5fa9c7d5cb405a344a86569d21a698c1a84e36dcdd3324c6c785c64459`:
  **art-share**, byte-identical to the compiled `sFossil_Pal`
  (`src/mirage_tower.c:77`, INCBIN of
  `graphics/object_events/pics/misc/fossil.gbapal`, cmp-verified; nm shows
  the hit inside `sFossil_Pal` at file offset 0x8cc300).
- `emerald:tileset/meteorfalls/palette/05` — sha
  `e52f9192018a822afada0c364c16293490a897cad6245dd11b1d8623e1ba732d`:
  **coincidence**, 4 zero + 12 shared BGR555 rock colors + 16 zero bytes
  inside the compiled `sSecretPowerCave_Pal` (`src/fldeff_misc.c:71`, nm-
  verified hit at file offset 0x6dd18e).

Both fire as NOTE (reported, not failed) in the CP8 battery isolation leg
(the scanned binary is byte-identical to the final `pokeemerald-linux64-
dinfo`, sha1 d792f8cf08ea332566d6356c2f396e7f82648a65); the full record
count and PASS line are unchanged (19953 ok, 4518/4518).

**Final release binary refinement:** against the final `-O3` release build
the two records pass with payloads **genuinely absent from the binary** —
`-O3` eliminates the unused static `sFossil_Pal` (nm: absent from the
release, present in DINFO), and the meteorfalls/05 byte pattern does not
occur anywhere in the release layout of `sSecretPowerCave_Pal` (nm:
present at 0x8d4920, but the -O0-era coincidence pattern is gone). No
exemption fires in the release scan; the sha-keyed exemptions remain in the
runner as the scan-time mechanism for the DINFO build and any non-stripped
build.

## 8. State v5

No format change, no serialized byte change. The neighborhood never appears
in a slice: host .data/.bss storage, outside BuildSlices' game-owned ranges
and the gba sections by construction (R6 trainer-table precedent). The two
hook edits are guarded (`#if NATIVE_LINUX` / `#if PLATFORM_SDL2 &&
NATIVE_LINUX`), touch neither slices nor BuildSlices, and the existing
state test suite stays byte-green (cross-restart ALL PASSED).

## 9. Build status (release + DINFO)

Both binaries rebuilt from the final tree with `-B` (the mandated forced
rebuild path — bare `.SECONDARY:` makes fresh native builds no-op):

- `pokeemerald-linux64` (release, DINFO=0): 30,251,488 bytes, **0 debug
  sections** (`readelf -S`), `-O3`, Build ID
  df8ee3b356a8c5e128c3823f46fc8ee26b63eb99.
- `pokeemerald-linux64-dinfo` (DINFO=1): 42,507,096 bytes, **7 debug
  sections** (`-g -O0 -rdynamic`), Build ID
  2d935e6f75ee5afee2c1c3f199018d0cc30c9fec, sha1
  d792f8cf08ea332566d6356c2f396e7f82648a65.

Build IDs differ as expected; module symbols nm-verified in both (5 of the
6 query symbols); isolation re-verified green (19953 ok, 0 failed,
4518/4518) against the final release binary (§7.1). `git diff --check`
clean.

## 10. Manual validation checklist (plan §14 — smoke gate)

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

## 11. Stop conditions — status

- Any checkpoint fails → none failed; all fixed with source-tree evidence.
- GBA parity → `git diff --name-only` outside src/platform/tests/docs is
  empty; the only production-file edits are the two guarded hook insertions
  in src/platform files (not compiled by the GBA build).
- Presentation path → zero E/F diff (§6).
- Second resource system / second pack path / pack format change / State v5
  format change / GBA behavior change / commit / push → none occurred.
- Battery red / isolation count change → 30/30 green; 19953/4518 unchanged.

**E/F implementation complete. STOP — awaiting manual validation (§10).**
