# R15 Final Content Closure Report

## Status: R15 COMPLETE

Every remaining R15 family is physically removed from the native link,
pack-served at runtime, and mechanically proven absent from the native
binary: UI graphics (374 ownership records), wallpapers (154), item
icons (467), battle-animation gfx (698), weather (1), easy-chat words
(1,008), and the Pokémon still-front / icon / footprint aux family
(2,826 payload resources total).

---

## UI GRAPHICS PHYSICAL REMOVAL (R15 Phase 8)

**Runtime publication.** All 94 mutable native UI structs (159 pointer
rows across 37 consumer files) are converted to the R15 pattern: on
NATIVE_LINUX the containing object is mutable and non-static, all scalar
metadata (sizes, tags, designated indices) is retained in the static
initializer, and only the pack-owned pointer fields start NULL; the GBA
branches are byte-identical to the pre-R15 sources. Name collisions
across TUs (sSpriteSheets, sSpritePalettes, sSpriteSheet_Confetti,
sSpritePalette_Confetti, sCompressedSpriteSheets) were resolved with
per-file renames on both branches plus all in-file uses.

**Generated publication.** `tools/gen3_resources/ui_family/` owns the
family end to end:
- `gen_ui_accessors.py` regenerates `ui_accessors.generated.h`: one
  accessor macro per symbol redirecting to `UI_Get()` plus a
  `<Symbol>_SIZE` literal (expected decoded size) so `sizeof(gSymbol)`
  call sites never degrade to pointer size; casts are derived from the
  checked-in GBA array element types. The five tileset-seam slots
  (gTilesetAnims_BattleDomePals0_*, gTilesetTiles_General) are excluded
  from the macros (tileset-family-owned).
- `gen_ui_populate.py` parses the converted consumer files and emits
  `emerald_ui_populate.c` + `ui_struct_population.h`: 159 assignments
  that reuse the original GBA initializer expressions verbatim (the
  accessor macros translate them into pack-backed pointers, including
  `&gPal[0x10]`-style offsets), plus the mirror NULL-ing
  `EmeraldUICompat_ClearStructPointers()`. Populate runs at the end of
  `EmeraldUICompat_TryInitialize`; clear runs on migrated-entry clear.
- `gen_ui_ownership.py` regenerates `ui/ownership.generated.toml` from
  the production manifest: 374 ROM_BASE_ONLY records (the five
  tileset-seam slots and the two fork-diverged compiled symbols are not
  records, R9 §6 policy). All three generators pass `--check`.

**Loader wiring.** The runtime loader publishes the seven R15 seams at
registration (easy-chat, wallpaper, item icon, battle-anim gfx, weather,
UI), all weak-probed so the offline harness links skip the section
(same shape as the R13-H3 state-adapter bridges). ADDITIVE DEGRADE
throughout: a failed publication leaves NULL pointers and a continuing
session.

**Physical removal.** All 379 canonical UI INCBIN payloads are gated
out of the native link with `#ifndef DESKTOP_EXTERNAL_GAME_CONTENT`
(graphics.c, pokemon.h); the two fork-diverged compiled symbols
(gNamingScreenCursor_Gfx, gPokedexBgHoenn_Pal) are not pack records.
The isolation runner's UI sweep proves: **canonical pack-owned UI
payload physically present in native binary = 0**, and
**UI COMPILED_PENDING_MIGRATION = 0** (374/374 ROM_BASE_ONLY isolated;
5 classified byte-scan exemptions: x86-instruction/affine-anim
coincidences plus the all-zero 2048-byte blank fill, each located in
the binary via readelf/nm before exemption).

---

## REMAINING FAMILIES CLOSED (Phases 2–7 completion)

**Pokémon still-front / icon / footprint (Phase 2/3).** The three
tables use the migrated macro split (SPECIES_STILL_SPRITE /
SPECIES_ICON / SPECIES_FOOTPRINT — native NULL sentinels, GBA
compiled); 1,213 payload INCBINs gated in pokemon.h + graphics.c; the
compat seam publishes slots 1760..3052 from the pack (still 440, icon
440, footprint 413); the one external still-front EGG row stays
compiled on every target and the generator maps both EGG rows to
POKEMON_BATTLE_EXTERNAL_SLOT (ownership excludes it, R9 §6).

**Wallpapers (Phase 6).** 154 records ROM_BASE_ONLY; all gWallpaper*
and sWallpaper* INCBINs and the 2D Walda palette arrays gated;
sSpriteSheet_Arrow publishes from `emerald:wallpaper/misc/arrow` with
the generated `sArrow_Gfx_SIZE`; accessor + SIZE macros wired into
graphics.h.

**Item icons (Phase 7).** 467 records ROM_BASE_ONLY; items.h gated via
`tools/gate_item_icons.py`; `gItemIconTable` mutable on native with the
two shared fallbacks (QuestionMark, ReturnToFieldArrow) staying
compiled and excluded from ownership + accessors; the compat seam
publishes every migrated slot.

**Battle-animation gfx (Phase 4).** 698 records ROM_BASE_ONLY; 633
payload INCBINs gated (graphics.c + battle_environment.h, including
comment-trailed definitions); gBattleAnimBackgroundTable,
sBallParticlePalettes and sBattleEnvironmentTable converted; accessor
macros wired into graphics.h with the compat declaration.

**Weather (Phase 1).** The 49,152-byte drought-color table is
pack-served into the mutable native `sDroughtWeatherColors` by
`EmeraldWeatherCompat_TryInitialize`; the record is ROM_BASE_ONLY.

**Easy-chat.** Untouched from the completed Phase B state (1,008 words
pack-served, `EC_GetWord()` live, 0 COMPILED_PENDING); regression legs
green.

---

## RANGE CLOSURE

The R15 still/icon/footprint extension grew the pokemon compat image
by 1,217 entries; `RebuildRangeIndex` registers one range per entry
(R10-F capture design), so the live range census moved exactly
6,390 → **7,608 / 8,192** (7,080 pre-script + 523 script modules + 5
live battle arenas). The loader gates (script 7,603, H 7,608) and the
harness fixture pins were updated to the new census; every R13-I range
invariant re-verified under the new total (identity unregister/restore,
clear/restage, per-family removal sequences).

---

## FINAL METRICS

| Metric | Value |
|--------|-------|
| Pack entries | 27,000 (cap: 32,768) |
| Pack bytes | 19,073,600 |
| Pack SHA-256 | `9815ba6fe81e3711f27ec7652f481a9dc2d2c74c161320ee2baca6f90e83b898` |
| ELF SHA-256 | `ce842ea2436df7ea72cda62d866a2a6946a1903cea2c0575673b3adc02731d84` |
| Deterministic | Yes (two `-B` rebuilds byte-identical) |
| Pack --check / generator --check | Passed (pokemon family suite 36 checks incl. the 2,826-resource --check + inventory no-op regen + completeness cross-check; UI accessors/populate/ownership byte-identical; wallpaper/item-icon/battle-anim regeneration stable) |
| --verify-game-data | Green: `f3ae088181bf583e55daf962a92bb46f4f1d07b7` |
| State-v5 | 5 (unchanged) |
| Ranges | 7,608 / 8,192 |
| Qualified ROM SHA-1 | `f3ae088181bf583e55daf962a92bb46f4f1d07b7` |
| Native asset isolation | 49,333 ok / 0 failed — 11,861/11,861 ROM_BASE_ONLY isolated across every family; 17/17 COMPILED_PENDING_MIGRATION present (15 movement STAY + 2 multiboot) |
| ASan / UBSan | resource-state sanitize + battle-live sanitize (oracle/faults/replace/state/battle/ai) clean |
| Forced release build | Green |
| Forced DINFO build | Green |
| R13 regression battery | Green — 26/26 legs, 0 failures (final run on the shipped tree) |
| R15 regression battery | Green (pokemon family 36 checks, UI generator checks, isolation runner, verify-game-data) |

---

## REGRESSION BATTERY

- R13 battery: 26 legs, 0 failures — state-master (5 legs),
  battle-state (+faults, cross-restart), battle-live,
  script/battle module loaders, script state (+faults,
  cross-restart), native-world real/render-proof/neighborhood,
  real-tables, runtime-loader (80,960 checks), resource-import,
  rom-base-provider, resource-ranges, resource-state,
  native-state-regression, layout/object-event/script compat,
  resource-lz, trainer-native-compat, overworld-renderer — green.
- R15 battery: pokemon-family generator suite (A–F, 36 checks incl.
  the 2,826-resource --check + inventory no-op regen), UI generator
  --checks, family regeneration stability, native asset isolation,
  --verify-game-data — green.

## HARD INVARIANTS (ALL PRESERVED)

- Qualified ROM SHA-1: `f3ae088181bf583e55daf962a92bb46f4f1d07b7`
- State-v5: 5
- Ranges: 7,608 < 8,192
- Pack: 27,000 < 32,768
- Forced build: `make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 -B rom`

## VERDICT

**R15 COMPLETE — Emerald asset/content ownership migration fully complete.**
