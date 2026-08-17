# R11-C Implementation Plan — Tileset Graphics / Palettes / Metatiles

Precise execution plan for the R11-C substage. All facts verified against the
current tree (2026-08-16, post-R11-B). Read
`docs/R11_OVERWORLD_RESOURCE_AUDIT.md` (§1.2 tilesets) and
`docs/R11B_OBJECT_EVENT_REPORT.md` (the machinery to reuse) before starting.

Scope: tileset tile graphics, tileset palettes, metatile definitions,
metatile attributes, tileset animation-frame leaves, native Tileset
publication, callback preservation, R10 v5/range integration, extraction/
pack/isolation/tests. OUT of scope: map blockdata, borders,
NativeWorldNeighborhood, camera work, R12 audio, R13 map/script/event data.

## 1. Leaf inventory (exactly which leaves migrate)

Source files: `src/data/tilesets/graphics.h`, `metatiles.h`,
`src/data/tilesets/headers.h` (all `#include`d by `src/tilesets.c:5-7`),
plus `src/graphics.c:1437-1457` (General tiles/palettes) and
`src/tileset_anims.c` (anim leaves).

| Kind | Total | Migrates | Stays GBA-parity only | Evidence |
|---|---|---|---|---|
| `gTilesetTiles_*` | 83 | **75** (one per `gTileset_*` struct) | 8 unreferenced: 6 `gTilesetTiles_SecretBase*Compressed` (graphics.h:684,706,728,750,772, +1), `gTilesetTiles_UnknownCableClub`, `gTilesetTiles_UnknownSecretBase` | no `gTileset_*` references them |
| `gTilesetPalettes_*[][16]` | 75 arrays = **1,200 raw 32-byte rows** | 75 arrays / 1,200 rows | none | graphics.h + graphics.c:1437-1457 |
| `gMetatiles_*` | 70 | 70 | none | metatiles.h |
| `gMetatileAttributes_*` | 70 | 70 | none | metatiles.h |
| anim-frame leaves `gTilesetAnims_*_FrameN` | 135 | 135 | none | tileset_anims.c:77-89+ |
| `sTilesetAnims_BattleDomeFloorLightPals` | 1 | 1 (classify at inventory time; treat as a raw u16 resource) | none | tileset_anims.c:540 |

Compression facts (verified):
- Tiles: 67 tilesets are `isCompressed = TRUE` (GBA LZ77 streams,
  `tiles.4bpp.lz`); 8 are `isCompressed = FALSE` (raw 16384-byte tiles):
  CableClub, SecretBase + its 6 color variants. Consumers:
  `CopyTilesetToVram` (fieldmap.c ~840-863) → `DecompressAndLoadBgGfxUsingHeap`
  (LZ77UnCompWram path — accepts literal-only LZ streams, the R9 precedent)
  or `LoadBgTiles` (raw path).
- Palettes: ALL raw 32-byte rows. `LoadTilesetPalette` (fieldmap.c:877-899):
  primary → raw row 0 (+1 black), secondary → raw row 6; the
  `LoadCompressedPalette` branch is dead FRLG legacy (`isSecondary` is bool8;
  no tileset reaches the third state). Do NOT create LZ palette resources.
- Metatiles/attributes: raw u16 blobs. Attributes pack behavior (bits 0-7) +
  layer type (bits 12-15); primary/secondary split at
  `NUM_METATILES_IN_PRIMARY` in `GetMetatileAttributesById`
  (fieldmap.c:409-424).
- Anim frames: raw 4bpp u16 leaves; consumed by the compiled
  `gTilesetAnims_*` frame tables + the DMA3 transfer machinery
  (tileset_anims.c:564-598, callbacks compiled).

**DECISION (item 4 of the mandate): animation-frame leaves are IN R11-C.**
The consumers are the compiled `gTilesetAnims_*` tables pointing at the leaf
symbols; publish-in-place (below) works identically to R11-B, the manual
gate includes animated tiles, and splitting them out would leave the
tileset family partially compiled for no consumer-path reason.

## 2. Native representation: publish-in-place, NO clones

**DECISION: keep the 75 compiled `struct Tileset` objects; do NOT clone them
into session-owned native-width objects.** Justification (the R11-B
precedent, audit §2.5): the tileset structs are C-compiled per target, so on
LINUX64 their pointer members ARE native-width already; the map layout
records reference the structs via `host_ptr` (layouts.inc), and every
consumer goes through `gMapHeader.mapLayout->primaryTileset/
secondaryTileset`. Cloning would add a second identity layer no consumer
reads. Required changes:

- Native branch of `headers.h`: `struct Tileset gTileset_X = {` (non-const,
  the R8S4a pattern) with the four payload members NULL-sentineled and
  `.callback = InitTilesetAnim_X` **kept compiled in the initializer**
  (callbacks MUST remain compiled function pointers — never migrated,
  never NULL).
- Native branch of `graphics.h`/`metatiles.h`/`tileset_anims.c` leaves: the
  INCBIN payloads become GBA-only; native gets non-const NULL-sentinel
  arrays (sizes retained), exactly like R11-B's generated header — emit via
  a generated `tileset_native.generated.h` with the same
  `OBJECT_EVENT_NATIVE_DEFINE`-style include-once toggle (DEFINITION in the
  seam TU, DECLARATIONS elsewhere). The anim-frame tables
  (`gTilesetAnims_*`) and all compiled callbacks stay compiled.
- The seam publishes: `.tiles`/`.palettes`/`.metatiles`/
  `.metatileAttributes` pointers into the session compat arena, and copies
  the raw bytes into the native palette-row/anim-frame arrays (the R11-B
  palette pattern). No native pointer ever enters a 4-byte GBA-shaped
  field (the GBA build keeps `struct Tileset` with 4-byte pointers; nothing
  is widened in place — the invariant holds by keeping the GBA data
  untouched and the native branch a separate declaration).

## 3. Canonical resource IDs / types / schemas / representations

Canonical component = lowercase symbol suffix, `_` → `-` (the R11-B rule).

| Resource | ID | Type | Schema | Source encoding | Representation | Expected size |
|---|---|---|---|---|---|---|
| compressed tiles | `emerald:tileset/<canonical>/tiles` | tile-graphics | 1 | gba-lz77 | gba-4bpp-tiles | 16384 decoded |
| uncompressed tiles | `emerald:tileset/<canonical>/tiles` | tile-graphics | 1 | raw | gba-4bpp-tiles | 16384 |
| palette row | `emerald:tileset/<canonical>/palette/<row 00-15>` | palette | 1 | raw | gba-bgr555-palette | 32 |
| metatiles | `emerald:tileset/<canonical>/metatiles` | tileset | 1 | raw | gba-metatile-defs | blob size (u16 words) |
| metatile attributes | `emerald:tileset/<canonical>/metatile-attributes` | tileset | 2 | raw | gba-metatile-attributes | blob size |
| anim frame | `emerald:tileset-anim/<canonical>/frame/<n>` | tile-graphics | 1 | raw | gba-4bpp-tiles | frame size |

Aliasing (one resource, multiple consumers — the R9 alias rule): the 6
SecretBase color variants share `gMetatiles_SecretBaseSecondary` /
`gMetatileAttributes_SecretBaseSecondary` → ONE `secret-base-secondary`
metatiles + one attributes resource, 6 consumer rows each. The 8
unreferenced parity leaves get NO canonical ids (they are removed from the
native link, GBA-only; document them in the ownership output as
`GBA_PARITY` rows).

elf_manifest additions (validation vocabulary ONLY — no pack format
change): `tileset ↔ gba-metatile-defs` and `tileset ↔
gba-metatile-attributes` rows in `TypeCompatibleWithRepresentation`
(manifest.c:134-140), mirroring the R11-B `sprite-sheet` addition. Total
new resources: 75 tiles + 1,200 palettes + 70 metatiles + 70 attributes +
135 anim frames + 1 floor-light-pals ≈ **1,551**.

## 4. Extraction / generator strategy (reuse R11-L machinery)

Mirror `tools/gen3_resources/object_event_family`:

1. `tools/gen3_resources/tileset_family/gen_tileset_inventory.py` — parse
   `headers.h` (75 structs: canonical, isCompressed, isSecondary, callback
   presence), `graphics.h` + `graphics.c` (tile leaves + 8 parity
   exclusions + 1,200 palette rows), `metatiles.h` (70 pairs + the 6-variant
   aliasing), `tileset_anims.c` (135 frames + per-tableset frame tables).
   Deterministic TOML + `--check`; duplicate/invalid-symbol detection.
2. `gen_tileset_family` (C, the gen_object_event_family skeleton) — validate
   every artifact (LZ decode 3-way for compressed tiles, raw byte identity +
   sizes for the rest, 32-byte palette rows, blob sizes from the committed
   bins), emit catalog/bindings/ownership/consumers + `tileset_native.
   generated.h` (NULL-sentinel native slots incl. the 75 structs) +
   `tileset_frames.generated.h` (publication rows).
3. `gen3-elf-manifest` with the qualified reference ELF
   (`../pokeemerald-reference/pokeemerald.elf`) + retail ROM, provenance
   string identical to R11-B's.
4. `gen3-pack-build` with the four manifests/catalogs (trainer, pokemon,
   object-event, tileset). Expected pack ≈ 2092 + 1551 = 3643 entries
   (import record cap 3072 will need raising → **do not forget**: update
   `EMERALD_IMPORT_MAX_RECORDS` and the two pinned cap tests in
   `emerald_resource_import_test.c` again). Rebuild, `--check`
   determinism, production proof pins updated.

## 5. Compat seam (R11-C publication lifecycle)

New `src/emerald/resources/emerald_tileset_compat.c` (+ header), the R11-B
contract:

- `EmeraldTilesetCompat_TryInitialize(snapshot, diagnostics)` —
  transactional: resolve + verify ALL resources (type/schema/winner==
  ROM_BASE/exact size), build ONE compat image (LZ entries for compressed
  tiles via `EMERALD_COMPAT_ENTRY_GBA_LZ` with expectedSize 16384, RAW for
  everything else), then publish: per-tileset struct field pointers +
  palette-row bytes + anim-frame bytes + the shared SecretBase
  aliases. Success clears diagnostics.
- `Republish` (allocation-free, idempotent — post-state-load re-derivation),
  `ClearMigratedEntries` (NULL the four struct pointer fields, zero palette
  rows + anim arrays), `Shutdown`, `GetImage`/`GetEntryCount`.
- Range registration (trainer seam `RebuildRangeIndex`): LZ tile streams →
  **ROLE_LEGACY_LZ** (they ARE the literal-LZ re-encoded representation);
  raw tiles/palettes/metatiles/attributes/anim frames →
  **ROLE_CANONICAL** (byte-identical canonical payloads). Never
  COMPAT_OBJECT for payload bytes.
- Lifecycle wiring (trainer seam): strict init after object-event
  (hard-fail with the compiled leaves gone), additive-degrade on the
  unit-harness path; republish/clear/shutdown delegated; `Makefile_pc`
  picks the TU up via the src wildcard.
- Callback preservation check: after TryInitialize,
  `gTileset_General.callback == InitTilesetAnim_General` etc. (pin in
  tests).

## 6. State v5 implications

- No tileset pointer enters any serialized slice today: the map grid holds
  u16 metatile ids, the structs live in host .data, layout records in
  .rodata reference image-range symbols. Nothing new is serialized.
- Cross-restart is the R11-B contract: post-load `Republish` re-derives
  every struct pointer and palette/frame byte array from the new process's
  arena. The published arena streams ARE registered in the R10 range index
  (roles above), so any future slice pointer into them is captured as
  identity+offset.
- Required tests: extend `tests/emerald_resource_state_test.c` (create/load
  modes) with post-load checks: `gTileset_General.tiles != NULL`,
  `gTilesetPalettes_General[0][0] != 0`, `gMetatiles_General[0] != 0`,
  and a frame-array sample — plus the existing rejection matrix staying
  green.

## 7. Isolation strategy

Disappear from the native link: 75 tile leaves, 1,200 palette rows, 70
metatile + 70 attribute blobs, 135 anim-frame leaves, the 8 parity leaves
(GBA-only, no canonical). Remain compiled (structural): the 75
`gTileset_*` structs, 31 `gTilesetAnims_*` tables, 25 `InitTilesetAnim_*` +
41 static `TilesetAnim_*` callbacks, the tilemap/anim machinery.
Isolation runner: add the tileset ownership file; expected exemptions to
document: 32-byte palette-row patterns coinciding with text padding (the
R11-B exemption mechanism, sha-keyed) and any shared-artifact consumers
found at scan time (verify with `grep -rln` before failing; report each).

## 8. Automated tests (new `tests/emerald_tileset_compat_test.c` + runner)

Synthetic full-family snapshot (pattern payloads, sizes from the generated
tables): all 75 structs published (pointer fields == arena streams, LZ
streams decode to the pattern), 1,200 palette rows byte-parity, 70+70
metatile/attribute blobs parity, 135 anim frames parity, primary/secondary
pairing (isSecondary/compressed flags pinned per struct), the 8
uncompressed tilesets served raw, callback pointers unchanged, shared
SecretBase aliasing, transactional failure (wrong-size tile), clear/
republish/shutdown/idempotence. ASan/UBSan variant. Cross-restart checks
(§6). Isolation extension. Full 13-runner battery + `git diff --check`.

## 9. Manual validation checklist

DINFO build; 1. Littleroot (primary + Petalburg secondary), 2. Route 101
(north connection), 3. Oldale, 4. interiors (Building tileset), 5. water
animation, 6. flowers/anim tiles, 7. doors/warps across primary+secondary
maps, 8. several transitions, 9. save state overworld → quit → relaunch →
load → tiles correct, 10. repeat. Pixel-identical to pre-R11 behavior.

## 10. Flash execution sequence (small checkpoints)

1. Read the three docs + gen_object_event_inventory.py +
   gen_object_event_family.c + emerald_object_event_compat.c (the
   templates).
2. Inventory script; run; `--check`; pin counts (75/1200/70/70/135).
3. Family generator; run; `--check`; inspect generated headers.
4. elf_manifest vocabulary rows; regenerate; `--check` both families.
5. Raise EMERALD_IMPORT_MAX_RECORDS (≥ 4096) + update the two pinned cap
   tests; run import test (155 green).
6. gen3-pack-build with 4 families; `--check`; production proof pins
   updated and green.
7. Seam implementation + generated-header include-once + headers.h/
   graphics.h/metatiles.h/tileset_anims.c native branches.
8. Trainer-seam wiring (init/republish/clear/shutdown/ranges); build
   DINFO; fix compile errors; self-test.
9. Compat test + runner; green + sanitize variant.
10. State-harness extension; green.
11. Isolation runner extension + exemptions; green.
12. Full battery + git diff --check; rebuild release + DINFO.
13. Report `docs/R11C_TILESET_REPORT.md` (the R11B report shape) +
    manual checklist. **STOP for manual validation.**

## 11. Stop conditions (hard stops)

- STOP before step 5 if the merged manifests would exceed the record cap
  without the cap change being test-pinned.
- STOP and report if any elf_manifest requirement cannot be met by
  vocabulary additions (a pack FORMAT change is forbidden without approval).
- STOP after step 12 if any regression fails.
- STOP after the report + manual checklist; do NOT begin map blockdata,
  borders, NativeWorldNeighborhood, camera work, or R12 until the R11-C
  manual gate is confirmed.
- Do not commit or push; do not change GBA behavior.
