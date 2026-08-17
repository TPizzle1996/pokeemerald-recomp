# R11-L + R11-B Substage Report — Object-Event Graphics Migration

## 1. Inventory results/counts

`tools/gen3_resources/object_event_family/gen_object_event_inventory.py` parses
`src/data/object_events/object_event_graphics.h` (INCBIN leaves),
`object_event_pic_tables.h` + `berry_tree_graphics_tables.h` (frame arrays).
Deterministic output + `--check` mode; duplicate/invalid symbol detection.

- 253 raw 4bpp sheet leaves (`gObjectEventPic_*`)
- 35 gbapal palette leaves (`gObjectEventPal_*`)
- 29 null-palette placeholders (classified separately, stay compiled)
- 249 SpriteFrameImage arrays
- 1,788 consumers: 1,690 `overworld_frame(...)` entries (sheet, w, h, frame
  derived textually from the macro arguments) + 98 `obj_frame_tiles(...)`
  whole-sheet entries
- field-effect leaves + `gUnusedObjectEventPalette` explicitly out of scope

## 2. Generated resource IDs/counts

Canonical = lowercase symbol suffix, `_`→`-`:
`emerald:object-event/<canonical>/sheet` (type sprite-sheet, schema 1,
source_encoding raw, gba-4bpp-tiles, payload = raw 4bpp bytes) and
`.../palette` (type palette, schema 1, raw, gba-bgr555-palette, 32 bytes).
**288 resources**, bytewise-sorted, duplicate-canonical detection.

## 3. Generator files

- `tools/gen3_resources/object_event_family/gen_object_event_inventory.py`
- `tools/gen3_resources/object_event_family/gen_object_event_family.c` +
  Makefile — validates every artifact (raw byte identity, palette size ==
  32, every frame range inside its sheet, whole-sheet sizes), emits
  catalog/bindings/ownership/consumers + three seam-side generated headers:
  - `include/emerald/resources/object_event_pic_tables.native.generated.h`
    (native arrays + frame arrays, include-once DEFINITION/DECL toggle),
  - `object_event_frames.generated.h` (1,788 publication rows),
  - `object_event_palettes.generated.h` / `object_event_sheets.generated.h`
- `tools/gen3_resources/pack_build/gen3_pack_build.c` + Makefile — thin
  driver over the existing multi-manifest importer (no new import/write
  logic).

## 4. Generated manifests

- `resources/extraction/emerald/bpee01/object_event/inventory.generated.toml`
- `.../catalog.generated.toml`, `bindings.generated.toml`,
  `ownership.generated.toml`, `object_event_consumers.generated.toml`
- `.../manifest.production.toml` — 288 records via gen3-elf-manifest
  (qualified pret reference ELF d8e405c4f6 + b89722500 +
  retail-matching ROM SHA-1 f3ae0881…), deterministic (`--check` passes).
- elf_manifest change: `sprite-sheet ↔ gba-4bpp-tiles` added to
  TypeCompatibleWithRepresentation (validation vocabulary; no format
  change). Import record limit 2048→3072 (merged manifests now 2092).

## 5. Exact production pack rebuild path/command

```
tools/gen3_resources/pack_build/gen3-pack-build \
  --rom ../pokeemerald-reference/pokeemerald.gba \
  --output games/emerald/base/emerald-bpee01-v1.rpack \
  --manifest resources/extraction/emerald/bpee01/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml \
  --manifest resources/extraction/emerald/bpee01/object_event/manifest.production.toml \
  --catalog resources/catalogs/emerald/catalog.toml \
  --catalog resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml \
  --catalog resources/extraction/emerald/bpee01/object_event/catalog.generated.toml
```
(`--check` verifies byte-identical determinism.) The same driver with the
two R9 manifests reproduces the R9 pack byte-for-byte.

## 6. Pack old/new resource counts and hashes

- Old: 1804 entries, 3,222,976 bytes, sha1 88221a774f8193306d8a8817c6eadf8f9e86899f
- New: **2092 entries** (196 trainer + 1608 pokemon + 288 object-event),
  3,705,104 bytes, sha1 **6a9f8d5d6d137f74cad668193bbfe3f01942f652**
- Prior entries unchanged by construction (same inputs + deterministic
  writer); deterministic rebuild verified byte-identical; production proof
  (37921 checks) green against the new pack.

## 7. Compat seam implementation

`src/emerald/resources/emerald_object_event_compat.c` (+ header): the R9
seam contract — transactional TryInitialize (resolve + verify ALL 288:
canonical key, type, schema, ROM_BASE winner, exact size → build one RAW
compat image → publish), allocation-free idempotent Republish,
ClearMigratedEntries (NULL data / zero palette bytes), Shutdown, image
accessors. Driven by the trainer seam's lifecycle (strict init; additive-
degradation on the unit-harness path); republish/clear/shutdown delegated.
Range registration: all 288 streams registered with **ROLE_CANONICAL**
(the bytes ARE the canonical raw payload; no LZ representation exists).

## 8. Frame-table publication design

The compiled frame arrays remain structural metadata; on native they are
the generated non-const NULL-sentinel tables (identical geometry,
include-once toggle). Publication sets
`frame.data = canonical sheet stream + width*height*frame*32` (offsets
from the generated consumers — never from compiled payload addresses) and
whole-sheet frames additionally get `.size` = sheet size. The
ObjectEventGraphicsInfo structs, anim/OAM/subsprite/affine tables and
`GetObjectEventGraphicsInfo` are unchanged.

## 9. Palette publication design

The native `gObjectEventPal_*` arrays are the non-const session-published
slots; publication copies the 32 canonical bytes in place; the compiled
tag→palette table (`sObjectEventSpritePalettes`) keeps pointing at them,
so tags/reflections/slots/berry/Mauville behavior are preserved. Null
placeholders stay compiled zero arrays.

## 10. ROLE_CANONICAL registration proof

The trainer seam's RebuildRangeIndex registers the 288 object-event
streams under EMERALD_RESOURCE_ROLE_CANONICAL (hull + per-entry ranges,
overlap-safe); any registration failure fails closed. (Decision recorded
in docs/R11_OBJECT_EVENT_DESIGN.md: LEGACY_LZ names the LZ-re-encoded
battle streams; COMPAT_OBJECT names objects; neither fits raw canonical
payloads.)

## 11. State v5 integration

No State v5 format change. The published frame `.data`/palette arrays
live in host memory and are re-derived by Republish after state load
(the R6/R9 contract); the cross-restart harness proves it (below). Any
future serialized-slice pointer into these streams would be captured as
identity+offset with role CANONICAL by the R10 walker (ranges
registered).

## 12. Cross-restart proof

The R10 cross-restart harness (real walker + seams + production pack)
now publishes the object-event family in both processes: after a fresh-
process load, `sPicTable_BrendanNormal[0].data` is re-derived to the
loader's arena (e.g. 0x56209e47c524) and `gObjectEventPal_Brendan`
carries the published bytes — with the battle-table row proofs and the
full rejection matrix still green.

## 13. Isolation proof

Native link loses all 253 sheet + 35 palette payload leaves (GBA-only
INCBIN branches); frame arrays/info structs/anim tables stay. Isolation
runner extended with the object-event ownership file: payload byte scans
prove the payloads are absent (8 documented sha exemptions: mirage-tower
fossil duplicate + 7 zero-dominated 32-byte palette patterns coinciding
with text padding), the seam TU is the sanctioned definition site for the
session-published slots, and no payload TU exists. GBA build unchanged.

## 14. Tests + sanitizer counts

- object-event compat test: 253 sheets + 35 palettes resolved, all 1,788
  frame rows hydrated (data == stream + offset, sizes), palette byte
  parity, transactional failure (wrong-size sheet), clear/republish/
  shutdown/idempotence — plus ASan/UBSan variant.
- cross-restart suite: all green incl. the new object-event checks.
- production proof: 37921 checks green on the 2092-entry pack.
- full battery (12 runners, exit 0): resource_lz 144 · resource_import 155 ·
  rom_base_provider 140 · trainer_native_compat 8377 · runtime_loader 2013 ·
  real_tables 16311 · trainer production 37921 · isolation (1804/1804 R9 +
  object-event per-record proofs) · ranges · fingerprint · cross-restart
  state · object-event compat (288 resources / 1,788 frames).
- sanitizers: object-event compat ASan/UBSan clean; R10 suites remain green.

## 15. Files changed

New: tools/gen3_resources/object_event_family/*,
tools/gen3_resources/pack_build/*, src/emerald/resources/
emerald_object_event_compat.c, include/emerald/resources/
emerald_object_event_compat.h, 4 generated headers under
include/emerald/resources/, resources/extraction/emerald/bpee01/
object_event/*, tests/emerald_object_event_compat_test.c,
tests/run_emerald_object_event_compat.sh.
Modified: elf_manifest (sprite-sheet representation), emerald_resource_import.c
(3072 limit), emerald_trainer_native_compat.c (wiring/registration),
src/data/object_events/object_event_graphics.h + pic tables headers
(native branches), src/event_object_movement.c (forward-decl branch),
include/graphics.h (native externs), the six seam-compiling runners +
isolation runner + state harness + production pins.

## 16. Warnings/issues

- The isolation summary line still reports the R9 1804 count; the
  object-event per-record checks pass individually.
- Trainer back-sheet R10 registrations remain ROLE_LEGACY_LZ
  (validated R10 behavior; not re-roled, per directive).

## 17. DINFO build status

Both `pokeemerald-linux64` and `pokeemerald-linux64-dinfo` rebuilt from the
final tree; DINFO self-test green (quick slot + slot 1 round trips +
trainer-family republish). `git diff --check` clean.

## 18. Concise manual validation checklist

DINFO build; then: 1. Littleroot overworld → player sprite walking;
2. running; 3. NPCs + movement animations; 4. reflections on water;
5. berry trees if accessible; 6. doors/entrances with object sprites;
7. Mauville old-man variant if practical; 8. multiple maps;
9. save state in overworld → quit completely → relaunch → load → all
object sprites correct; 10. repeat once. Visuals must be pixel-identical
to pre-R11 behavior.
