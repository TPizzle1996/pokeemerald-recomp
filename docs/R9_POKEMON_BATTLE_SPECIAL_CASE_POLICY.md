# R9 Stage 6 — Pokémon battle graphics special-case policy

Status: complete. Every special case named in the R9 directive (Unown, Castform,
Deoxys, Spinda, Egg, shiny, Substitute) was investigated against the generated
mapping (`include/emerald/resources/pokemon_battle_slots.generated.h`) and the
retail data/consumer sources. The policy is:

> **All special cases are data, not code.** The compatibility seam
> (`emerald_pokemon_native_compat.c`, R9 §5) has zero handwritten per-species
> logic. Every alias, size, and external slot comes from the generated slot map
> and resource table, which the family generator emits from the retail tables'
> extraction metadata. All consumer-side post-decode behaviors (Deoxys tile
> duplication, Spinda spots, Castform form palettes, Unown letter mapping,
> shiny selection, the >NUM_SPECIES fallback) are untouched and run byte-for-byte
> identically, because the seam serves streams that decode to exactly the retail
> decode (pinned three-way in Stage 3, re-encoded by Gen3LzLiteral_Encode in §5).

One row — the back-EGG row — is permanently external: it aliases a payload from
the *still-front* table's namespace and stays compiled forever. Substitute and
icons are out of scope, verified not to alias the four tables.

## 1. Species and slot numbering

`POKEMON_BATTLE_SLOTS_PER_TABLE` = 440, slot index == species id
(`include/constants/species.h`): SPECIES_NONE = 0, SPECIES_UNOWN = 201,
SPECIES_OLD_UNOWN_B = 252 … SPECIES_OLD_UNOWN_Z = 276, SPECIES_CASTFORM = 351,
SPECIES_SPINDA = 327, SPECIES_JIRACHI = 409, SPECIES_DEOXYS = 410,
SPECIES_CHIMECHO = 411, SPECIES_EGG = 412, SPECIES_UNOWN_B = 413 (NUM_SPECIES +
1) … SPECIES_UNOWN_Z = 439. `NUM_SPECIES` = 412, so every Unown form slot is
`> NUM_SPECIES`, which several consumer paths branch on.

## 2. The full aliasing picture (verified from the generated header)

The family's 1608 resources map to 1760 slots. Slot->resource inspection found
exactly **six** multi-slot alias groups, plus one cross-kind alias (shiny-EGG)
and the slot-0 fallback. No other species shares graphics with any other — in
particular nothing aliases outside the four tables except the one external row.

| case | slots | kinds | shared resource(s) | retail source |
|---|---|---|---|---|
| OLD_UNOWN_B..Z | 252..276 (25) | front, back, normal, shiny | `question_mark/double/{front,back,normal-palette,shiny-palette}` (r1038..1041) | `front_pic_table.h:263-287`, `back_pic_table.h:263-287` literally place `gMonFrontPic_DoubleQuestionMark` / `gMonBackPic_DoubleQuestionMark` in all 25 rows |
| Unown palettes | 201 + 413..439 (28) | normal, shiny | `unown/battle/{normal-palette,shiny-palette}` (r1422, r1423) | shared for letter A (slot 201, personality path) and letters B..Z (form slots, anim path) |
| Unown sheets | 201 + 413..439 | front, back | per-form: `unown/{a..z}/battle/{front,back}/sheet`, 26 pairs (front 4096, back 2048) | `LoadSpecialPokePic_2` maps `SPECIES_UNOWN` via personality to slots 413..439 |
| Slot 0 (SPECIES_NONE) | 0 | front, back, normal, shiny | `question_mark/circled/*` | live fallback for `species > NUM_SPECIES` in every load path |
| Egg shiny | shiny 412 | shiny | `egg/battle/normal-palette` (r329) | retail `shiny_palette_table.h:423`: `SPECIES_SHINY_PAL(EGG, gMonPalette_Egg)` — the shiny row *is* the normal palette artifact; modeled as one resource aliased by both slots |
| Egg back | back 412 | back | **POKEMON_BATTLE_EXTERNAL_SLOT** | retail `back_pic_table.h:423`: `SPECIES_SPRITE(EGG, gMonStillFrontPic_Egg)` — payload is a still-front asset, not a battle-family resource |

Single-slot rows of note (sizes are the mapping's decoded sizes, automatically
validated by §5 Phase 1 and `CreateFamily`'s LZ77-header check):

- **Castform (351)**: front sheet 8192 (4×2048 frames), back sheet 8192
  (4×2048 frames), normal palette 128 (4×32 sub-palettes), shiny palette 128.
- **Deoxys (410)**: front 4096 (2×2048 frames), back 4096 (2 frames), palettes
  32.
- **Spinda (327)**: front 4096 (2 frames), back 2048 (1 frame), palettes 32.
- **Egg (412)**: front 4096 + normal palette 32 (canonical), back external,
  shiny = normal (cross-kind alias above).
- **Slot 0**: circled question mark, all four kinds — a consumer path, not a
  stub; it must publish like every other migrated slot (it does).

## 3. Consumer-side behaviors (all unchanged)

Complete enumeration of direct consumers of the four tables
(`grep -rn 'gMon…Table\[' src/ --include='*.c'`): `decompress.c`,
`battle_gfx_sfx_util.c`, `egg_hatch.c`, `evolution_scene.c`, `trade.c`,
`contest_painting.c`, `pokemon_summary_screen.c`, `trainer_pokemon_sprites.c`,
`pokemon.c`. Every access is by species index on a table row, or on `.data`;
nothing stores a `.data` pointer across a load, and the single pointer-identity
check (`decompress.c:77`, `src == &gMonFrontPicTable[species]`) compares row
*addresses*, which publication never changes (only `.data` pointers).

- `DecompressPicFromTable[2]`, `LoadSpecialPokePic[2]` and the
  `_DontHandleDeoxys` / `HandleLoadSpecialPokePic*` variants:
  `LZ77UnCompWram(table[species].data)`; `species > NUM_SPECIES` →
  `gMonFrontPicTable[0].data` (slot 0 fallback).
- **Deoxys**: `DuplicateDeoxysTiles` (`decompress.c:407`) — after decode, for
  SPECIES_DEOXYS only, `CpuCopy32(frame1 → frame0, MON_PIC_SIZE)`. The 4096-byte
  sheet is two frames; the consumer renders frame 1. `_DontHandleDeoxys`
  variants skip the duplication. Nothing to do in the seam.
- **Spinda**: `DrawSpindaSpots` (`pokemon.c:5781`) — `SPECIES_SPINDA &&
  isFrontPic` only; `DRAW_SPINDA_SPOTS(personality, dest)` draws the four
  personality-derived spot pairs onto the decoded front buffer. Back sprites
  carry no redrawn spots (retail behavior). Nothing to do in the seam.
- **Unown**: `LoadSpecialPokePic_2` (`decompress.c:314-331`) — for
  SPECIES_UNOWN, `GET_UNOWN_LETTER(personality)`; letter 0 → slot 201,
  otherwise `letter + SPECIES_UNOWN_B - 1` → slots 413..439; then
  `gMonFrontPicTable[i]` / `gMonBackPicTable[i]`. `battle_anim_mons.c` uses the
  same form-slot arithmetic directly. Palettes: `GetMonSpritePalFromSpeciesAnd
  Personality` uses slot 201 on the personality path and form slots on the anim
  path — both covered by the shared palette rows.
- **Castform**: `battle_gfx_sfx_util.c:615-619` — `LZDecompressWram(gMonPalette
  Table[351].data)` into `gBattleStruct->castformPalette`, then
  `LoadPalette(castformPalette[gBattleMonForms[battler]], …)`. The 128-byte
  payload is four 32-byte sub-palettes selected after decode; frames are fixed
  animation frames within the 8192-byte sheets. Sheet/palette `.size` and
  `.tag` are retained by §5, so whole-sheet allocation is unchanged.
- **Shiny selection**: `GetMonSpritePalFromSpeciesAndPersonality` /
  `GetMonSpritePalStructFromOtIdPersonality` (`pokemon.c:6517-6543`) —
  `GET_SHINY_VALUE(otId, personality) < SHINY_ODDS` → `gMonShinyPaletteTable
  [species]`, else `gMonPaletteTable[species]`; `species > NUM_SPECIES` →
  `gMonPaletteTable[SPECIES_NONE].data`. Covers shiny Unown (shared shiny row),
  shiny Egg (normal-palette alias), and the slot-0 fallback.

## 4. Out of scope (verified not in the four tables)

- **Substitute**: `gSubstituteDollFrontGfx` / `gSubstituteDollBackGfx` /
  `gSubstituteDollPal` are compiled INCBIN assets
  (`graphics.c:853`, consumed `battle_gfx_sfx_util.c:1038-1050`). Not in the
  four tables; nothing to migrate.
- **Icons / menu sprites**: `pokemon_icon.c` owns the icon family
  (`gMonIcon_*`, 440 entries). No icon symbol appears in the four battle table
  headers, and no battle symbol appears in `pokemon_icon.c` — the families are
  disjoint; icons stay compiled.
- **Still-front table**: `gMonStillFrontPicTable`
  (`still_front_pic_table.h`, consumed `pokemon_jump.c:2731`) is a separate
  440-row table of its own symbols. The only cross-family touch is the battle
  back-412 row, which aliases `gMonStillFrontPic_Egg` — that is exactly why
  that slot is external. The still table and its leaves stay compiled.

## 5. Why the seam needs no special-case code

1. The slot map is generated from the retail tables' extraction metadata; the
   aliasing it encodes *is* the retail aliasing (multi-slot rows, cross-kind
   shiny-EGG, the one external row). `PublishPokemonTables` /
   `ClearMigratedEntries` already publish every slot from the same resource row
   and skip the external sentinel generically.
2. `expectedSize` (4096/2048/8192/128/32) comes from the mapping; §5 Phase 1
   requires the snapshot payload size to equal it, and `CreateFamily` requires
   the re-encoded stream's LZ77 header to declare it. The multi-frame and
   multi-palette payloads are validated by the same generic checks as every
   other row.
3. The generator fails on unresolved, uncovered, or unreferenced slots, so any
   future retail aliasing flows through the generated header automatically;
   regenerating is a data change, never a code change.
4. The one permanent compiled dependency — `gMonStillFrontPic_Egg`
   (`src/data/graphics/pokemon.h:2715`, `INCBIN graphics/pokemon/egg/front.4bpp.lz`)
   — is the back-412 external slot's payload and must survive R9 Stage 7
   (also used by still-table row 418).

## 6. What R9 Stage 8 must pin

- All 1760 slots publish; the back-412 external slot stays at
  `gMonStillFrontPic_Egg` (non-NULL, compiled, untouched by
  `ClearMigratedEntries`).
- Slot-0 fallback: a `species > NUM_SPECIES` load decodes the circled-question-
  mark payload.
- Unown: `LoadSpecialPokePic_2` with SPECIES_UNOWN + personality letters
  (including letter 0 → slot 201) decodes the per-form sheets; the shared
  normal/shiny palettes serve slots 201 and 413..439.
- Old Unown: slots 252..276, all four kinds, decode to the double-question-mark
  payloads.
- Castform: sheets decode to 8192 (4 frames), palettes to 128 (4 sub-palettes);
  form sub-palette selection is consumer-side.
- Deoxys: sheets decode to 4096 (2 frames); `DuplicateDeoxysTiles` runs on the
  load path.
- Spinda: front decodes to 4096 and spots draw via `DrawSpindaSpots`.
- Shiny Egg: slot 412's shiny row decodes to the *normal* egg palette payload
  (cross-kind alias).
