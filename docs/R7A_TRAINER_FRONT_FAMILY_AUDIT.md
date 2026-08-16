# Stage R7A — Complete Trainer-Front Graphics Family Audit

Date: 2026-08-14 · Branch: `agent/prepare-v0.1.0-alpha` · Game: Emerald (BPEE, rev 0)

This document is the §1 audit report for Stage R7A: the complete trainer-front
graphics family (front sheets + front palettes), produced **before** any broad
metadata edit. It records the enumeration of the family from the authoritative
source artifacts and tables, the size proofs, the palette sharing cases, the
consumer inventory, and the canonical-name rule. Everything here is derived
mechanically from the checked-in sources listed in the "Sources" section; the
one-time audit script (`/tmp/r7a_audit.py`, not committed) parsed those sources
and decoded every artifact with the GBA LZ77 decoder that mirrors
`src/platform/bios.c` `LZ77UnCompWram`.

---

## 1. Scope (Stage R7A §2)

**In scope.** Every trainer **front** sheet and every trainer **front** palette:

- `gTrainerFrontPicTable[]` — 93 sheet entries (indices 0..92).
- `gTrainerFrontPicPaletteTable[]` — 93 palette entries (indices 0..92).
- The 5 front palettes that live under `graphics/trainers/palettes/` rather than
  `graphics/trainers/front_pics/` (brendan, may, wally, brendan_rs, may_rs).
- The 6 front palettes that are also consumed from `gTrainerBackPicPaletteTable[]`
  (recorded as *additional consumers* of the same front palette resource; the
  back *sheets* are out of scope and are **not** migrated).

**Out of scope.** Trainer back sheets, back-only palettes
(`gTrainerBackPicPalette_Red/Leaf`), Pokémon sprites, overworld graphics,
title/UI, battle backgrounds, fonts, tilesets, audio, maps, scripts, dialogue.

## 2. Enumeration

The family is enumerated from three checked-in sources (see §8):

1. `include/constants/trainers.h` — 93 `TRAINER_PIC_*` constants, `HIKER=0` ..
   `RS_MAY=92`.
2. `src/data/trainer_graphics/front_pic_tables.h` — the two tables, in constant
   order, with per-entry size multiplier.
3. `src/data/graphics/trainers.h` — the INCBIN declarations mapping each symbol
   to its source artifact.

Result: **93 front table entries, 93 unique sheets, 93 unique palettes.**
Each table entry maps one-to-one to one sheet resource and one palette resource.
There are no NULL-sentinel entries other than the native-target Brendan slots
(which are R6 runtime-published, metadata retained); the GBA branch has all 93
compiled. No front sheet is shared between table entries, and no front palette
symbol is shared between table entries.

## 3. Size proofs (§6)

Every sheet LZ stream decodes to **exactly 2048 bytes** and every palette LZ
stream decodes to **exactly 32 bytes**, verified by decoding all 93 sheets and
all 93 palettes from their committed artifacts with the R1A/`bios.c`-compatible
strict GBA LZ77 decoder.

### 3.1 The `.size` field is consumer allocation metadata, not payload size

Six table entries carry `TRAINER_PIC_SIZE * 2` (= 4096) in the table's `.size`
field while their LZ payload still decodes to 2048 bytes:

| TRAINER_PIC | index | table .size |
|---|---|---|
| SR_AND_JR | 50 | 4096 |
| POKEFAN_M | 51 | 4096 |
| CHAMPION_WALLACE | 54 | 4096 |
| CYCLING_TRIATHLETE_M | 56 | 4096 |
| BATTLE_GIRL | 64 | 4096 |
| ARENA_TYCOON_GRETA | 85 | 4096 |

`src/decompress.c` `LoadCompressedSpriteSheet` copies `src->size` into
`dest.size` and feeds the payload to `LZ77UnCompWram`, which sizes the output
from the LZ header — confirming the decoded size comes from the stream header,
not the table field. So the canonical decoded size for **every** front sheet is
2048 bytes and for **every** front palette is 32 bytes, independent of the table
multiplier. The exact-size constraint can therefore be expressed once per type
(family-wide invariants), and the 2× entries remain *consumer allocation*
metadata recorded in the descriptor, not a payload-size distinction.

## 4. Palette byte-sharing — semantic distinctness (§12)

Seven groups of distinct symbols decode to byte-identical palettes. They are
**not** merged: identity is semantic, and each symbol is a distinct compiled
resource with its own consumers and table slot.

| Group | distinct symbols | decoded bytes identical? |
|---|---|---|
| aqua_grunt_m, leader_roxanne | 2 | yes |
| expert_m, expert_f, old_couple | 3 | yes |
| leaf, red | 2 | yes |
| magma_admin, magma_grunt_f, magma_grunt_m | 3 | yes |
| running_triathlete_f, swimming_triathlete_f, swimming_triathlete_m | 3 | yes |
| sis_and_bro, swimmer_f | 2 | yes |
| tuber_f, tuber_m | 2 | yes |

17 trainers in 7 groups. All 17 stay separate resources (§12). No sheet bytes
are duplicated across distinct symbols.

## 5. Front palettes with additional (back-pic-palette) consumers (§13)

The GBA `gTrainerBackPicPaletteTable[]` references **front palette resources** in
6 of its 8 slots; Red and Leaf use their own back-only palettes
(`gTrainerBackPicPalette_Red/Leaf`, out of scope).

| front palette resource (canonical) | GBA back-table slot |
|---|---|
| brendan | `gTrainerBackPicPaletteTable[0]` (player gender live consumer) |
| may | `gTrainerBackPicPaletteTable[1]` |
| ruby-sapphire-brendan (constant `RS_BRENDAN`) | `gTrainerBackPicPaletteTable[4]` |
| ruby-sapphire-may (constant `RS_MAY`) | `gTrainerBackPicPaletteTable[5]` |
| wally | `gTrainerBackPicPaletteTable[6]` |
| steven | `gTrainerBackPicPaletteTable[7]` |

These are recorded as *additional consumers* of the one front-palette resource;
the back *sheets* (`gTrainerBackPicTable[]`) are untouched and not migrated.

## 6. Consumer inventory (§13)

### 6.1 `gTrainerFrontPicTable` (sheets)

- `src/field_effect.c:897-910` — `CreateTrainerSprite` /
  `LoadTrainerGfx_TrainerCard` (`gTrainerFrontPicTable[trainerSpriteID / gender]`).
- `src/battle_gfx_sfx_util.c:704` — `DecompressPicFromTable_2(&gTrainerFrontPicTable[frontPicId], ...)`.
- `src/pokenav_match_call_gfx.c:1250` — `DecompressPicFromTable(&gTrainerFrontPicTable[trainerPic], ...)`.
- `src/trainer_pokemon_sprites.c:82` — `DecompressPicFromTable(&gTrainerFrontPicTable[species], ...)`.
- `src/emerald/resources/emerald_trainer_native_compat.c` — native-only runtime
  publication of the Brendan slot (R6 seam).

### 6.2 `gTrainerFrontPicPaletteTable` (palettes)

- `src/battle_controller_player.c:2329` — tag lookup (opponent trainer pic palette tag).
- `src/battle_controller_opponent.c:1318,1388` — tag lookups.
- `src/battle_controller_link_opponent.c:1297,1320` — tag lookups.
- `src/battle_controller_recorded_opponent.c:1247` — tag lookup.
- `src/battle_controller_player_partner.c:1332,1802` — tag lookup + palette load.
- `src/battle_controller_recorded_player.c:1232` — tag lookup.
- `src/field_effect.c:896,899,911` — `CreateTrainerSprite` / trainer-card palette.
- `src/battle_gfx_sfx_util.c:707,726` — load + free by tag.
- `src/pokenav_match_call_gfx.c:1251` — `LZ77UnCompWram` into match-call gfx.
- `src/trainer_pokemon_sprites.c:114,119,129` — trainer-card mon sprites palette.
- `src/emerald/resources/emerald_trainer_native_compat.c` — native-only Brendan slot.

### 6.3 `gTrainerBackPicPaletteTable` (the 6 shared front palettes' extra consumers)

- `src/battle_controller_player.c:2963` — `[gSaveBlock2Ptr->playerGender]` (live: brendan/may).
- `src/battle_controller_link_partner.c:1561` — `[trainerPicId]`.
- `src/battle_controller_player_partner.c:1797` — `[spriteId]`.
- `src/battle_controller_recorded_player.c:1681` — `[trainerPicId]`.
- `src/battle_controller_wally.c:1448` — `[TRAINER_BACK_PIC_WALLY]` (wally palette).
- `src/battle_gfx_sfx_util.c:716` — `[backPicId]`.

No consumer is changed in R7A (§13 "Do NOT change consumers").

## 7. Canonical-name rule (§4)

Canonical trainer component = ASCII-lowercase of the `TRAINER_PIC_*` constant
suffix with underscores converted to hyphens:

```
TRAINER_PIC_HIKER                -> hiker
TRAINER_PIC_AQUA_GRUNT_M         -> aqua-grunt-m
TRAINER_PIC_LEADER_TATE_AND_LIZA -> leader-tate-and-liza
TRAINER_PIC_RS_BRENDAN           -> rs-brendan
TRAINER_PIC_RS_MAY               -> rs-may
TRAINER_PIC_BRENDAN              -> brendan
```

The rule is deterministic and uniform; the descriptor
(`resources/extraction/emerald/bpee01/trainer_front_family.toml`) is the single
explicit, reviewable mapping from constant/table identity to canonical name and
is checked in. Full resource ID pattern (§3):

```
emerald:trainer/<canonical>/battle/front/sheet
emerald:trainer/<canonical>/battle/front/normal-palette
```

186 canonical IDs (93 sheets + 93 palettes). The two Brendan IDs reproduce the
R6 IDs verbatim; their M0/M1 keys reproduce the R6 keys verbatim (verified:
`95821423…` and `6d7aec37…`).

## 8. Sources and reproducibility

- `include/constants/trainers.h`
- `src/data/trainer_graphics/front_pic_tables.h`
- `src/data/trainer_graphics/back_pic_tables.h`
- `src/data/graphics/trainers.h`
- `graphics/trainers/front_pics/` (93 `.4bpp.lz` + 88 `.gbapal.lz`)
- `graphics/trainers/palettes/` (5 `.gbapal.lz`)
- `src/decompress.c`, `src/platform/bios.c` (LZ77 / `.size` semantics)
- `src/emerald/resources/resource_id.c` (key derivation)

Every numeric claim above (counts, sizes, indices, duplicates) was recomputed by
the audit script against these sources on 2026-08-14.

## 9. Counts summary (§20)

| item | count |
|---|---|
| front table entries | 93 |
| unique front sheets | 93 |
| unique front palettes | 93 |
| shared sheets | 0 |
| shared palettes (by symbol) | 0 |
| palette byte-duplicate groups (semantically distinct) | 7 groups / 17 trainers |
| front palettes with back-pic-palette consumers | 6 |
| resources already `ROM_BASE_ONLY` (native) | 2 (Brendan sheet + palette) |
| resources `COMPILED_PENDING_MIGRATION` | 184 |
