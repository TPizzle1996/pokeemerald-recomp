# R11-D: Raw map blockdata — plan

Stage: R11-D of the overworld resource migration (R11_OVERWORLD_RESOURCE_AUDIT §2.1:
blockdata + border leaves "in scope per R11-D").
Scope: migrate `data/layouts/*/map.bin` (blockdata) and `border.bin` (border
words) into the resource system. Nothing else moves.

Method: mirror R11-C (tilesets) exactly — same emission pattern, same compat
seam lifecycle, same publication mechanics. R11-C is the proven template for
moving bytes out of the native link while keeping the GBA build byte-identical.

---

## 1. Leaf inventory (verified, corrects the audit)

| Leaf | Count | Bytes | Notes |
|---|---|---|---|
| blockdata (`map.bin`) | 441 | w×h×2 each (30×30 Petalburg = 1800 B); **20 layouts deviate** (18 unused 1×1 → 4 B, CaveOfOrigin_Unused_B4F_Lava 724 vs 722, LittlerootTown_ProfessorBirchsLabWithTable 340 vs 338) | payload = **file bytes verbatim** (the GBA build INCBINs the file); largest 6400 blocks (Underwater_Route127 80×80), smallest 1 block |
| border (`border.bin`) | 441 | 8 B each (4 u16, 2×2) | **32 distinct hashes** (116× 9ff63640…, 105× efef8d5a…, 35× 0c2871e5…, 26× a8fb987b…, …) — the audit's "4 u16 uniform" is wrong |

- `layouts.json` = 441 records (id/name/width/height/primary_tileset/
  secondary_tileset/border_filepath/blockdata_filepath).
- Border storage decision: **441 resources, one per layout, no dedup**. The
  duplication costs 441×8 B ≈ 3.5 KB in the pack; the R9 alias machinery is for
  mapping data, not pack byte savings. The 32-distinct fact is recorded in the
  inventory and report for documentation.
- No `[[gba_parity]]` rows: blockdata/border are raw (uncompressed) on both
  targets — no LZ split.

## 2. Publish-in-place: YES via seam-TU records (not session views)

The audit said "struct MapLayout views — hydrate into session storage" because
the layout records are `.rodata` in the native binary. R11-C proved the
mechanism that makes records writable without touching game source: the compat
seam TU **redefines** the records as writable C objects (the
`TILESET_NATIVE_STRUCT` pattern), and every other TU gets extern declarations.

Same for R11-D:

- **`data/maps.s` (via mapjson-generated `layouts.inc`)**: on native
  (`LINUX64` defined in ASFLAGS — Makefile_pc:95), the payload `.incbin`s and
  the layout record definitions are **skipped entirely** — the payload symbols
  have **no direct C references** (verified: no `*_Blockdata` / `*_Border`
  symbol use in `src/`; all access flows through `mapLayout->map`/`->border`),
  so no zero-filled shells are needed (unlike R11-C anim frames, which are
  referenced by symbol). GBA build (`LINUX64` undefined) emits the old bytes
  unchanged.
- **`layouts_table.inc` stays untouched** — its `gba_ptr` entries are
  four-byte symbolic references; on native they resolve to the seam-TU record
  symbols (non-PIE, < 4 GiB — `HostResolveGbaAddr` contract, host_memory.c:325).
- **`layout_native.generated.h`** (from the family generator): `LAYOUT_NATIVE_DEFINE`
  in the seam TU defines all 441 records as writable
  `struct MapLayout <name> = {.width=…, .height=…, .border=NULL, .map=NULL,
  .primaryTileset=&gTileset_…, .secondaryTileset=&gTileset_…}`; every other
  includer gets extern declarations. (The tileset pointers come from
  R11-C's `tileset_native.generated.h` extern declarations.)
- **No game-source changes at all**: `GetMapLayout()` (overworld.c:549),
  battle_pyramid.c:1540, and every `gMapHeader.mapLayout->map/border` consumer
  (fieldmap.c:54-59 macros, MapGridGetMetatileIdAt/BehaviorAt/LayerTypeAt,
  region_map.c, overworld.c — 34/13/10 sites) keep reading the resolved
  record; the record *is* the writable seam object whose pointers were
  published from the pack.

## 3. Canonical IDs and types

`GEN3_RESOURCE_TYPE_TILEMAP` already exists in the runtime vocabulary
(resource_types.h:16) — no type-system change.

| Leaf | ID | Catalog type | Schema |
|---|---|---|---|
| blockdata | `emerald:layout/<slug>/blockdata` | `tilemap` (verify TOML string) | 1 |
| border | `emerald:layout/<slug>/border` | `tilemap` | 1 |

`<slug>` follows the tileset convention (`BattleArena` → `battlearena`):
lowercased layout name minus the `_Layout` suffix (verify against
`gen_tileset_family.c` id derivation; determinism check in checkpoint 2).

## 4. Extraction / generation strategy

- **`tools/gen3_resources/layout_family/`** (new, mirrors tileset_family):
  - `gen_layout_inventory.py`: reads `data/layouts/layouts.json`, scans the
    882 `.bin` files → `inventory.generated.toml` (882 records: id/key/symbol/
    type/source_artifact/encoded+decoded length/`raw` encoding/sha256s/
    ownership_state `ROM_BASE_ONLY` for native, `COMPILED` for gba —
    `[resources.targets]` per R11-C).
  - `gen_layout_family.c`: inventory + layouts.json → `catalog.generated.toml`,
    `bindings.generated.toml` (canonical_representation `gba-tilemap`),
    `ownership.generated.toml`, `layout_consumers.generated.toml` (441 layout
    records with tileset refs), `layout_native.generated.h`,
    `layout_frames.generated.h` (publication macro rows: symbol + blockdata/border
    resource ids + sizes). Same build isolation as R7A §24 / R11-C: `-std=c99
    -Wall -Wextra -Werror`, Gen3 core (sha256/resource_id/util/toml/lz77).
- **Output dir**: `resources/extraction/emerald/bpee01/layout/`.
- **mapjson** (`tools/mapjson/mapjson.cpp` `generate_layout_headers_text`,
  lines 590-621): per layout, wrap payload `.incbin`s and the record emission
  in `.if LINUX64` … `.else` … `.endif` (GBA = current bytes verbatim;
  native = comment only). Regeneration must produce a no-op diff for the GBA
  path.

## 5. elf_manifest vocabulary

`manifest.c` `TypeCompatibleWithRepresentation` (lines 137-148) gains one row:

```
if (strcmp(catalogType, "tilemap") == 0)
    return strcmp(canonicalRepresentation, "gba-tilemap") == 0;
```

New representation string `gba-tilemap` = raw little-endian u16 entries
(blockdata/border are stored uncompressed; `source_encoding "raw"`).
`--check` must be a no-op diff after regenerating the layout manifest.

## 6. Import cap + production pack

- `EMERALD_IMPORT_MAX_RECORDS` 4096u → **8192u** (emerald_resource_import.c:56;
  pack grows 3636 → 4518 records; comment update). `MAX_PAYLOAD` 16 MiB
  unchanged (largest blockdata 12.8 KB).
- Pack rebuild: the R11-C §5 command (gen3-pack-build with base + pokemon_battle
  + object_event + tileset manifests/catalogs) gains the layout pair:
  `--manifest resources/extraction/emerald/bpee01/layout/manifest.production.toml
  --catalog resources/extraction/emerald/bpee01/layout/catalog.generated.toml`.
- Expected: 4518 entries; pack hash changes; `--check` byte-identical
  determinism.

## 7. Compat seam

`src/emerald/resources/emerald_layout_compat.c` (mirrors emerald_tileset_compat.c):

- `#define LAYOUT_NATIVE_DEFINE` + `#include "emerald/resources/layout_native.generated.h"`.
- `BuildEntries()`: 882 rows (per layout: blockdata id+size, border id+size)
  from the generated frames header.
- `TryInitialize`: transactional — verify all 882 entries present in the
  snapshot, `payloadSize == expectedSize`,
  `winningProviderId == EMERALD_ROM_BASE_PROVIDER_ID`; build image; publish
  only on success (fail closed, nothing mutated).
- `PublishFromImage()`: per layout, `record.map = FindStream(blockdataId)`,
  `record.border = FindStream(borderId)` — pointers into session memory
  (same as R11-C tiles/metatiles/attrs).
- `ClearMigratedEntries()`: `record.map = NULL; record.border = NULL`.
- `Republish()` idempotent (allocation-free); `Shutdown()`; `GetImage()`;
  `GetEntryCount/GetEntrySchema/GetEntryRole` (all canonical →
  `EMERALD_RESOURCE_ROLE_CANONICAL` — no LZ rows).
- Guard `#if PLATFORM_SDL2 && NATIVE_LINUX`; GBA build never compiles it.

## 8. Lifecycle wiring

`emerald_trainer_native_compat.c` (the umbrella driver):

- Strict path (after the R11-C tileset block): layout family not published →
  init error ("R11-D compiled leaves are gone") — blockdata absence makes maps
  unrenderable.
- Additive unit-harness path: missing family → NULL-sentinel warning, status OK
  (mirror the R11-C opt-in block).
- `Republish`, `ClearMigratedEntries`, `Shutdown` calls alongside the tileset
  family's.

## 9. State v5

The layout image is session-scoped: it is rebuilt from the snapshot at
restore (TryInitialize → RebuildRangeIndex flow), so record pointers re-derive
post-load exactly like the tileset family. No v5 format change. Proof: the
cross-restart test (checkpoint 10) must show `mapLayout->map`/`->border`
pointing into the new session arena after load.

## 10. Isolation

- Ownership union gains the 6th file
  (`resources/extraction/emerald/bpee01/layout/ownership.generated.toml`).
- No `[[gba_parity]]` strip needed (raw rows).
- Object scan: native `maps.o` must not contain any blockdata/border byte
  sequences (the `.if LINUX64` skip guarantees it; the runner proves it).
- Preprocessed-TU check: `layouts.inc` is ASM (not a C TU) — the runner's C-TU
  check does not cover it; the object scan covers maps.o instead.
- Sha-keyed byte exemptions: all 882 payloads exist in the ROM (they are
  INCBIN'd there), so the ROM-BASE comparison behaves like every other
  ROM_BASE_ONLY row.

## 11. Tests

- New `tests/run_emerald_layout_compat.sh` mirroring
  `tests/run_emerald_tileset_compat.sh` (TryInitialize transactional failure
  modes, Republish idempotence, ClearMigratedEntries, ROLE_CANONICAL,
  size-mismatch diagnostics).
- Cross-restart state proof (checkpoint 10) extended with layout arena lines.
- Full battery: all 20 existing scripts must stay green (isolated run — the
  layout seam is additive).

---

## Checkpoint sequence (flash, in order; stop on any failure)

1. `gen_layout_inventory.py` → 882-record inventory (deterministic;
   regeneration is a no-op diff). Record the 32-distinct-border analysis.
2. `gen_layout_family.c` → catalog/bindings/ownership/consumers/native header/
   frames header; determinism check; `<slug>` convention verified against
   R11-C ids.
3. mapjson `.if LINUX64` emission change; regenerate `layouts.inc`;
   GBA-path output byte-identical to pre-change (diff the LINUX64=0 output);
   native output skips payloads + records, keeps table refs.
4. elf_manifest `tilemap` vocabulary row; layout bindings;
   `manifest.production.toml` (882 records) + `--check` no-op.
5. Import cap → 8192; production pack rebuild (4518 entries) + `--check`;
   record pack hash.
6. Seam TU + generated-header include; umbrella driver wiring (strict +
   additive paths).
7. Native rebuild (release + DINFO); verify records published
   (`mapLayout->map`/`->border` point into session arena; no dangling refs);
   battle_pyramid floor accessor works.
8. `run_emerald_layout_compat.sh` green.
9. State v5 cross-restart proof (layout pointers re-derived, arena lines
   recorded).
10. Isolation runner update (6-file union) + proof; object scan clean.
11. Full battery (all 20 + new test).
12. `git diff --check`; release (0 debug) + DINFO (7 debug) verified via
    `readelf -S`; distinct Build IDs.
13. Write `docs/R11D_MAP_BLOCKDATA_REPORT.md` (R11C report shape) with 14-item
    manual validation checklist; STOP for manual validation.

## Stop conditions

- Any checkpoint fails → fix with source-tree evidence; no plan redesign
  without evidence.
- GBA build parity broken (layouts.inc diff on the GBA path) → stop.
- Second resource system / second pack path / pack format change / State v5
  format change / GBA behavior change / commit / push: all prohibited.
