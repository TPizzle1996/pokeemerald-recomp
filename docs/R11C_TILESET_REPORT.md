# R11-C Report — Tileset Graphics Migration

## 1. Inventory results/counts

`tools/gen3_resources/tileset_family/gen_tileset_inventory.py` parses
`src/data/tilesets/graphics.h` (INCBIN leaves per family), `headers.h`
(compiled `struct Tileset` records), `metatiles.h` (metatile + attribute
arrays) and `src/tileset_anims.c` (anim frame INCBINs + frame tables).
Deterministic output + `--check` mode; per-family ownership/symbol
validation.

- 75 compiled `struct Tileset` objects (general + secondary families)
- 75 tile sheets (raw 16 KiB 4bpp), 1,200 palette rows (32-byte gbapal
  rows, one per compiled palette array), 70 metatile arrays, 70
  metatile-attribute arrays
- 125 anim frames (31 frame tables) + 4 battle-dome floor-light palettes
- **1,544 resource leaves**, 150 consumer rows (frame-table → leaf
  bindings), 18 `[[gba_parity]]` records (unreferenced leaves with no
  canonical — see section 20)
- The battle-dome floor-light palette arrays `gTilesetAnims_BattleDomePals0_*`
  are parity-free: all 4 rows are referenced by the dome anim tables and
  become canonical resources

## 2. Generated resource IDs/counts

Canonical = lowercase symbol suffix, `_`→`-`, under family-scoped
namespaces:

- `emerald:tileset/<family>/tile` (type sprite-sheet, schema 1, raw,
  gba-4bpp-tiles, 16384 bytes) — 75
- `emerald:tileset/<family>/palette/<row>` (type palette, schema 1, raw,
  gba-bgr555-palette, 32 bytes) — 1,200
- `emerald:tileset/<family>/metatile` (type gba-metatile-defs, 70) and
  `.../metatile-attribute` (type gba-metatile-attributes, 70) — the two
  new elf_manifest representations (section 4)
- `emerald:tileset-anim/<family>/<anim>/frame/<n>` (type sprite-sheet,
  raw, gba-4bpp-tiles) — 125
- `emerald:tileset-anim/battle-dome/floor-light-pal/<n>` (type palette,
  32 bytes) — 4

**1,544 resources**, bytewise-sorted, duplicate-canonical detection.

## 3. Generator files

- `tools/gen3_resources/tileset_family/gen_tileset_inventory.py` —
  inventory/consumer extraction (deterministic, `--check`)
- `tools/gen3_resources/tileset_family/gen_tileset_family.c` + Makefile —
  validates every artifact (raw byte identity, tile sheet size == 16384,
  palette row size == 32, frame ranges inside their family's anim dir,
  LZ77-compressed tile three-way validation via the strict shared
  decoder), emits catalog/bindings/ownership/consumers + two seam-side
  generated headers:
  - `include/emerald/resources/tileset_native.generated.h` (native
    non-const payload arrays + NULL-sentinel sizes, include-once
    DEFINITION/DECL toggle via `TILESET_NATIVE_DEFINE`),
  - `include/emerald/resources/tileset_frames.generated.h` (31 frame
    tables + 150 consumer rows)
- Build isolation per the R7A §24 precedent: `-std=c99 -Wall -Wextra
  -Werror`, no Emerald defines; links the exact M0/M1 key-derivation code
  so ownership keys are recomputed with the same algorithm.

## 4. Generated manifests

- `resources/extraction/emerald/bpee01/tileset/inventory.generated.toml`
- `.../catalog.generated.toml`, `bindings.generated.toml`,
  `ownership.generated.toml` (1544 resources + 18 `[[gba_parity]]`
  blocks), `tileset_consumers.generated.toml`
- `.../manifest.production.toml` — 1544 records via gen3-elf-manifest
  (qualified pret reference ELF d8e405c4f6 + b89722500 + retail-matching
  ROM SHA-1 f3ae0881…), deterministic (`--check` passes).
- elf_manifest vocabulary additions (checkpoint 4): `gba-metatile-defs`
  and `gba-metatile-attributes` canonical representations for the 140
  metatile/attribute leaves (validation vocabulary; no format change).
- Import cap 3072→4096 (`EMERALD_IMPORT_MAX_RECORDS`; merged
  manifest/catalog views now hold 3643 records, 4096 headroom).

## 5. Exact production pack rebuild path/command

```
tools/gen3_resources/pack_build/gen3-pack-build \
  --rom ../pokeemerald-reference/pokeemerald.gba \
  --output games/emerald/base/emerald-bpee01-v1.rpack \
  --manifest resources/extraction/emerald/bpee01/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/object_event/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/tileset/manifest.production.toml \
  --catalog resources/catalogs/emerald/catalog.toml \
  --catalog resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml \
  --catalog resources/extraction/emerald/bpee01/object_event/catalog.generated.toml \
  --catalog resources/extraction/emerald/bpee01/tileset/catalog.generated.toml
```
(`--check` verifies byte-identical determinism.) The same driver with
fewer manifests reproduces each prior pack byte-for-byte.

## 6. Pack old/new resource counts and hashes

- Old: 2092 entries, 3,705,104 bytes, sha1 6a9f8d5d6d137f74cad668193bbfe3f01942f652
- New: **3636 entries** (196 trainer + 1608 pokemon + 288 object-event +
  1544 tileset), 5,220,944 bytes, sha1 **14eb935e3c341bca3a337115b5799ae505d8363c**
- Prior entries unchanged by construction (same inputs + deterministic
  writer); deterministic rebuild verified byte-identical; production proof
  (37921 checks) green against the new pack.

## 7. Compat seam implementation

`src/emerald/resources/emerald_tileset_compat.c` (+ header), 644 lines:
the R9 seam contract — transactional TryInitialize (429) resolves and
verifies ALL 1544 records (canonical key, type, schema, ROM_BASE winner,
exact size) and publishes the 75 compiled `struct Tileset` payload members
(`.tiles`, `.palettes`, `.metatiles`, `.metatileAttributes`) from the
session arena; allocation-free idempotent Republish (566),
ClearMigratedEntries (581, NULL data members), Shutdown (611). Driven by
the object-event seam's lifecycle (strict init; additive-degradation on
the unit-harness path).

Native representation (plan §2): the 75 structs are the compiled objects
(no clones); `headers.h`'s native branch is non-const structs whose payload
members are the generated NULL-sentinel arrays from
`tileset_native.generated.h` (DEFINITION in this TU via
`#define TILESET_NATIVE_DEFINE`, DECLARATIONS elsewhere);
`graphics.h`/`metatiles.h`/`tileset_anims.c` INCBIN payloads are GBA-only
(`#if !defined(NATIVE_LINUX)`), native links the generated arrays.

## 8. Frame-table publication design

The compiled anim frame tables (`gTilesetAnims_*`) stay structural
metadata. On native they reference the generated non-const NULL-sentinel
frame arrays (identical geometry); the 31 frame tables are re-published
from `tileset_frames.generated.h` (150 consumer rows) with
`frame.data = canonical stream + offset` and `.size` set, so the
`callback = InitTilesetAnim_X` compiled anim drivers render from
session-owned canonical bytes. `gTilesetAnims_Sootopolis_StormyWater`
frames are two-artifact concatenations (INCBIN kyogre + groudon) — the
ownership record carries `source_artifact_2` so the canonical bytes match
the compiled leaf exactly (section 19).

## 9. Palette publication design

The 1,200 native palette rows are the non-const session-published slots;
publication copies the 32 canonical bytes in place into each compiled
palette array, preserving per-palette row indexing, transparency slot 0
and the compiled `gTilesetAnims_BattleDomePals0_*` dome palette rows
(4 canonical floor-light palettes). `.callback` anim drivers and palette
re-pointing (`TilesetCB_*`) are unchanged.

## 10. ROLE_CANONICAL registration proof

All 1544 streams registered under EMERALD_RESOURCE_ROLE_CANONICAL
(emerald_tileset_compat.c:638/641 — hull + per-entry ranges,
overlap-safe); any registration failure fails closed. Rationale as in
R11-B: the bytes ARE the canonical raw payloads; no LZ representation
exists for tiles/palettes/attributes, and metatile/attribute defs have no
compressed form at all.

## 11. State v5 integration

No State v5 format change. Published `.tiles`/`.palettes`/
`.metatiles`/`.metatileAttributes` members live in host memory and are
re-derived by Republish after state load (the R6/R9 contract); the
cross-restart harness proves it (below). Future serialized-slice pointers
into these streams would be captured as identity+offset with role
CANONICAL by the R10 walker (ranges registered).

## 12. Cross-restart proof

The R10 cross-restart harness (real walker + seams + production pack)
publishes the tileset family in both processes; the loader process
re-derives the four pinned struct pointers to its own fresh arena:

```
tileset arena: general=0x55e38a09a558 petalburg=0x55e38a0fbe80 pal[0]=0x28a3 flower[0]=0xdddd
tileset arena: general=0x564e4d67c558 petalburg=0x564e4d6dde80 pal[0]=0x28a3 flower[0]=0xdddd
```

Different heap bases across processes, identical published bytes
(`pal[0]=0x28a3`, `flower[0]=0xdddd` are the canonical first-row values)
— no stale pointers survive a state load.

## 13. Isolation proof

Native link loses all 1544 payload leaves. Isolation runner (now driven
by the 3636-record union of all five ownership files) proves it three
ways: (a) payload byte scans over the binary — 0 unexpected hits after
the 11 sha-keyed exemption groups (466 records total: all-zero 32-byte
palette rows and metatile/floor-light attrs that coincide with text
padding, plus 8 art-sharing coincidences verified with `grep -rln`
against their compiled twins: sSecretPowerPlant_Pal, sHofMonitor_Pal,
gTradeGba_Gfx, gMimicOrbAffineAnimCmds1, gFieldEffectPal_SmallSparkle,
sTrainerHillWindowTilemap, gMonFootprint_Metang, gMonIcon_Armaldo);
(b) object scan (no payload TU exists; the seam TU is the sanctioned
definition site); (c) native dependency-graph check via the build's own
preprocessed TUs (section 22). **16425 ok, 0 failed; PASS 3636/3636
ROM_BASE_ONLY isolated (196 + 1608 + 288 + 1544), 0
COMPILED_PENDING_MIGRATION.** GBA build unchanged.

## 14. Tests + sanitizer counts

- tileset compat test: 75 structs / 1544 entries ALL PASSED (real seam +
  generated tables + unit-harness stubs).
- cross-restart suite: all green incl. the new tileset arena checks.
- production proof: 37921 checks green on the 3636-entry pack.
- full battery (20 runners, exit 0): resource_lz 144 · resource_import
  155 · rom_base_provider 140 · trainer_native_compat 8377 ·
  runtime_loader 2013 · real_tables 16311 · trainer production 37921 ·
  native asset isolation 16425 · ranges · fingerprint · cross-restart
  state · object-event compat · tileset compat (75 structs / 1544
  entries).
- sanitizer variants: resource_import_sanitize 155, resource_lz_sanitize
  144, rom_base_provider_sanitize 140, trainer_native_compat_sanitize
  8377, native_overworld_sanitize — all exit 0.
- `git diff --check` clean.

## 15. Files changed

New: tools/gen3_resources/tileset_family/* (inventory + generator +
Makefile), src/emerald/resources/emerald_tileset_compat.c,
include/emerald/resources/emerald_tileset_compat.h,
include/emerald/resources/tileset_native.generated.h +
tileset_frames.generated.h, resources/extraction/emerald/bpee01/tileset/*
(7 generated manifests), tests/emerald_tileset_compat_test.c,
tests/emerald_tileset_compat_stubs.c, tests/run_emerald_tileset_compat.sh,
docs/R11C_TILESET_PLAN.md.
Modified: src/tileset_anims.c (10 parity leaves GBA-only, section 21),
src/data/tilesets/graphics.h + metatiles.h + headers.h (native branches,
NULL-sentinel payloads), include/tilesets.h, elf_manifest (metatile
vocabulary), emerald_resource_import.c (4096 cap),
emerald_trainer_native_compat.c + emerald_object_event_compat.c
(lifecycle wiring), emerald_resource_session.c (arena sizing),
tests/run_emerald_native_asset_isolation.sh (3636-record union +
preprocessed-TU check, sections 20/22), tests/emerald_resource_state_test.c
(cross-restart pins).

## 16. Warnings/issues

- 18 `[[gba_parity]]` records (unreferenced leaves with no canonical:
  Lava frames 4-7, Unused1 frames 0-3, Unused2 frames 0-1, plus
  zero-padding rows); they are GBA-only (section 21) and stripped from
  the isolation scan (section 20).
- 11 sha-keyed byte exemptions (section 13) are coincidental byte
  identity with compiled data — documented per-sha with reasons in the
  runner; re-verified with grep before classification.
- Build-system quirk discovered during the DINFO rebuild (not an R11-C
  regression): `Makefile_pc` line 181 `.SECONDARY:` marks all files
  secondary, so missing object files are treated as up-to-date — a fresh
  `make -f Makefile_pc linux64` no-ops unless forced with `-B`. All
  rebuilds in this report used `-B`.

## 17. DINFO build status

Both binaries rebuilt from the final tree with `-B`:

- `pokeemerald-linux64` (release): 30,794,432 bytes, 0 debug sections,
  Build ID e242831815a7208adf58a65b785d2d056773e31f — isolation re-verified
  green (16425 ok, 0 failed) against it.
- `pokeemerald-linux64-dinfo` (DINFO=1): 43,037,208 bytes, 7 debug
  sections, `-g -O0 -rdynamic`, Build ID
  e4d0d8fda34381dbcb9784eea76f3baee0a03a23.

Build IDs differ as expected; `git diff --check` clean.

## 18. Concise manual validation checklist

DINFO build first (section 17); then with the release binary:

1. Littleroot Town overworld — primary general tileset renders
2. Petalburg City — secondary tileset renders
3. Route 101 north connection — tiles correct across the connection
4. Oldale Town — another map for general-tileset coverage
5. An interior (e.g. the player's house) — Building tileset renders
6. Water animation — animating water tiles (wave/flow) correct
7. Flowers/anim tiles — animated tile frames advance
8. Doors/warps on both a primary-tileset and a secondary-tileset map
9. Several map transitions (route→town→interior→route)
10. Save state in the overworld → quit completely → relaunch → load →
    all tiles correct (no garbage, no stale graphics)
11. Repeat once
12. Visuals pixel-identical to pre-R11 behavior
13. Palette slots/transparency correct (no washed-out tiles)
14. DINFO build also boots to the overworld without errors

## 19. R11-C addition — `source_artifact_2` concatenation fix

Two-artifact leaves (Sootopolis StormyWater frames INCBIN kyogre +
groudon, 8 records) dropped their second artifact from the ownership
records in the first generator cut — the isolation byte scan failed 24
stormy-water records. The generator now emits `source_artifact_2` inside
the `[[resources]]` record (before `[resources.targets]`), the runner
concatenates it for byte scans, and the canonical bytes match the
compiled leaf exactly. Verified: `grep -c source_artifact_2` = 8,
ownership + production manifests both 1544 records.

## 20. R11-C addition — `[[gba_parity]]` parser fix

The tileset ownership file appends 18 `[[gba_parity]]` blocks after the
1544 `[[resources]]` records; the isolation runner's generic TOML split
treated them as resources and failed 466 records with corrupt
source paths. The runner now strips at the first `[[gba_parity]]`
boundary (same for all five ownership files; only tileset emits parity
blocks). Parity leaves are excluded from the scan — they exist GBA-only
and are never linked natively.

## 21. R11-C addition — parity-frame guard fix

10 unreferenced anim frames (Lava 4-7, Unused1 0-3, Unused2 0-1) were
compiled into the native binary unguarded (found via the preprocessed
`tileset_anims.i` — 10 `data/tilesets` hits). Each is now wrapped in
`#if !defined(NATIVE_LINUX)`, keeping the filler arrays
(`tileset_anims_space_11` etc.) native as before. Verified absent from
the rebuilt binary (Lava/4 128B and Unused2/1 96B gone; Unused1/0
presence is vacuous all-zero padding).

## 22. R11-C addition — native preprocessed-TU dependency check

scaninc has no `-D` flag and evaluates `#if !defined(NATIVE_LINUX)` as
TRUE, so its closure is the GBA-branch union — false positives on every
in-place GBA-guarded INCBIN. Replaced with exact native ground truth:
the runner extracts each TU's cpp command from the forced make dry-run
(`make -f Makefile_pc linux64 -nB`), replaces `-o <path>` with `-o -`,
runs it via the make-shell quoting, and greps the preprocessed output for
migrated artifacts. Preprocessed graphics.c/data.c contain 0
`data/tilesets` hits (tileset_anims.c: 10 — the section 21 bug).

## 23. R11-C addition — generator determinism

`gen-tileset-family --check` re-derives all 7 outputs from the inventory
and compares bytewise (deterministic no-op when unchanged). Verified
after the section 19 generator edit: rebuild + `--check` green; the
regeneration run reported "wrote 1544 resources (75 tiles, 1200 palette
rows, 70 metatiles, 70 attributes, 125 anim frames, 4 floor-light), 75
structs, 31 frame tables, 150 consumers, 18 parity" and is repeatable.

## 24. Expanded manual checklist (superset of section 18)

The section 18 list is the release gate. Add, when practical:
15. battle-dome water tile (gTilesetAnims_BattleDome*) animates in the
    Battle Frontier dome if reachable
16. Sootopolis stormy water frames animate (kyogre/groudon concatenated
    frames)
17. any map whose tileset pairs a secondary family with palette
    re-pointing (TilesetCB_*) — no palette glitch on entry
18. load a save state taken mid-animation (water animating) — anim
    resumes from published frames, not garbage.
