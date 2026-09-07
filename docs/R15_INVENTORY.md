# R15 — Exact Inventory and Ownership Plan (Phase 1)

Machine-readable companion: `/tmp/r15-inventory.tsv` (family | symbol | size | artifact | rom_offset).
Qualified ROM SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`; every payload below is
byte-verified against it (rom_offset >= 0) unless stated.

## Reconciliation vs R14 approximations (qualified truth wins)

| Family | R14 approx | R15 exact | Encoding | ROM provenance |
|---|---:|---:|---|---|
| mon icons | 430,080 B | 412 sym / 421,888 B | RAW 4bpp (1024 B) | all in-ROM |
| still fronts | 363,816 B | 415 sym / 363,232 B | gba-lz77 (decoded 2048 B) | all in-ROM |
| footprints | 12,384 B | 386 sym / 12,352 B | RAW 1bpp (32 B) | all in-ROM |
| battle-animation gfx | 118,988 B | 633 sym / 179,180 B | gba-lz77 + raw pals | all in-ROM |
| item icons | 57,896 B | 218 icons 42,888 B + 253 pals 8,896 B = 51,784 B | gba-lz77 | all in-ROM |
| wallpapers | 35,700 B | 94 sym / 33,268 B | gba-lz77 + raw pals | all in-ROM |
| UI/misc | 322,000 B | 353 sym / 221,824 B (4 dome-anim pals excluded — already TOML-declared) | mixed | in-ROM (4 exceptions are the declared dome pals) |
| credits text | 2,256 B | 140 sym / 2,256 B | poketext | in-ROM |
| union-room text | 13,297 B | 193 sym / 7,933 B | poketext | in-ROM |
| decoration text | 5,310 B | 120 sym / 5,310 B | poketext | in-ROM |
| weather LUT | 49,152 B | 49,152 B | RAW | in-ROM (0x54014c) |
| money/coords | 314 B | 314 B | — | **NOT in ROM** (fork-extended/diverged) → NATIVE_DIVERGENCE, not pack targets |
| easy-chat | 1,008 words | 1,008 (already in pack, COMPILED_PENDING) | text | — |
| multiboot | 176,352 B | EReader 12,512 B live / Colosseum 163,840 B dead | RAW | in-ROM (both) |

## Resource models (Phase 1A)

- icons / still fronts / footprints → extend the R9 pokemon family (same generator + seam;
  keys `emerald:pokemon/<slug>/icon`, `.../battle/front/still`, `.../footprint`).
  Egg still-front = external slot (stays compiled, documented — no canonical artifact).
- battle-anim gfx → new family `emerald:battle-anim-gfx/...` (graphics only; no H VM changes).
- item icons → `emerald:item-icon/...` + palettes.
- wallpapers → `emerald:wallpaper/<name>/{tiles,tilemap,frame-palette,bg-palette}`.
- UI → decomposed named subfamilies (`emerald:ui/<subsystem>/<name>`), no generic bucket.
- credits/union/decor text → text-family bundles + skeleton-published pointer tables
  (the R13-C live-skeleton pattern).
- easy-chat → text resources already packed; runtime repoint of the 45 group tables
  (R13-D/E pattern) + ownership flip.
- weather LUT → `emerald:data/weather/drought-colors` RAW static resource, host fill.
- multiboot → EReader: pack-derived at load (live consumer cutover); Colosseum:
  excluded from the native link entirely (natively dead, `#ifndef PORTABLE`).

## Range projection (Phase 1B)

- per-entry ranges: battle-anim gfx 633 (live during battle; mid-battle save safety) = +633.
- arena/span ranges: still fronts + icons + footprints (one aux pokemon image,
  arena-span registered — transient consumers), item icons, wallpapers, UI, text
  bundles, easy-chat, weather ≈ +15 (transient consumers; no capturable state holds
  their pointers; arena-offset identity is the H-family precedent).
- Projected live ranges ≈ 6,390 + 633 + 15 ≈ **7,038 / 8,192** (under cap, no cap change).
- State-v5: version 5 unchanged; no format/cap change; no new raw pointer persistence.

## Pack projection

- New resources ≈ 412 + 415 + 386 + 633 + 218 + 253 + ~130 (wallpaper parts) + ~150
  (UI parts) + ~6 (text bundles) + 1 (weather) ≈ **2,604**.
- Projected pack ≈ 23,069 + 2,604 ≈ **25,673 < 32,768** (under cap, no cap change).

## Battle-animation gfx family (Phase 4) — plan

- 633 payload leaves (all LZ-encoded: sprite .4bpp.lz, pal .gbapal.lz, bg image
  .4bpp.lz, bg tilemap .bin.lz) — 179,180 B canonical, all in-ROM.
- Keys: `emerald:battle-anim-gfx/<name>` (+ implicit palette pairing by table
  row shape).
- Consumer surface: gBattleAnimPicTable (CompressedSpriteSheet rows {gfx,size,tag}),
  gBattleAnimPaletteTable (CompressedSpritePalette rows {pal,tag}),
  gBattleAnimBackgroundTable (BattleAnimBackground rows {image,pal,tilemap}),
  plus direct table initializers in battle_anim_throw.c (sBallGfxTable),
  battle_anim_rock.c, battle_anim_mons.c, battle_anim_water.c, battle_intro.c,
  battle_bg.c, evolution_scene.c, battle_anim_utility_funcs.c.
- Model: same seam pattern as the R15 pokemon aux family — tables mutable on
  native with NULL rows, published from a new compat image at init;
  per-entry ranges (mid-battle live; role LEGACY_LZ).
- VM note: no H VM changes; Cmd_loadspritegfx keeps reading the published
  tables by index.

## Battle-anim gfx — implementation notes

- Inventory generator: tools/gen3_resources/battle_anim_family/gen_battle_anim_gfx.py
  (633 payloads, 289 pic rows, 289 pal rows, 27 bg rows — exact, tested).
- Table shapes: gBattleAnimPicTable (CompressedSpriteSheet {gfx,size,tag}),
  gBattleAnimPaletteTable (CompressedSpritePalette {pal,tag}),
  gBattleAnimBackgroundTable (BattleAnimBackground {image,palette,tilemap},
  BG_* designated).
- All payloads LZ-encoded (u16 ×3 unused bins included).
- Seam: new emerald_battle_anim_gfx compat (arena image, per-entry ranges,
  role LEGACY_LZ); tables mutable on native with NULL rows; the 8
  consumer files' local initializer tables get NULL rows + seam-published
  pointers (battle_anim_throw/rock/mons/water/intro/bg/evolution_scene/
  utility_funcs).
