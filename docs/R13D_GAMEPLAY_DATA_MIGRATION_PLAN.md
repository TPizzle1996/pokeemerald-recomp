# R13-D — Emerald Gameplay Data + content.pak Absorption Migration Plan

Scope: design review only. This document settles exact R13-D ownership and
resource granularity, the content.pak absorption, publication transforms,
consumer seams, State-v5 impact, and isolation for the gameplay-data stage.
**No implementation. No commit. R13-E not started.**

Grounding: `docs/R13_REMAINING_CONTENT_AUDIT.md` (R13-A),
`docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md`,
`docs/R13B_LEAF_PAYLOAD_MIGRATION_REPORT.md`,
`docs/R13C_TEXT_MIGRATION_PLAN.md` (+ `R13C_AUDIT_CORRECTIONS.md`),
`docs/R13C_TEXT_MIGRATION_REPORT.md`,
`docs/EMERALD_ROM_BACKED_ASSET_MIGRATION_ARCHITECTURE.md`,
`docs/R10_NATIVE_STATE_V5_DESIGN.md`, and a fresh audit of the R13-C-complete
tree (tag `checkpoint-r13c-complete`, HEAD `2533bfa46`).

Method: every figure in §1 was recomputed against the qualified pret
reference build (`../pokeemerald-reference/pokeemerald.elf` +
`pokeemerald.gba`, BPEE01 Rev 0, SHA-1
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`) with `readelf -sW` symbol sizes
and ROM-slice arithmetic, then cross-checked against the recomp tree's
compiled/hydrated representations. Where this plan disagrees with the R13-A
audit, the recomputed figure is authoritative and the delta is called out.

Current proven state entering R13-D:

| Quantity | Value |
|---|---|
| Production pack `games/emerald/base/emerald-bpee01-v1.rpack` | 12,063 entries / 12,100,064 B / SHA-256 `a57b51be17f8e942…47a784` |
| Text pins (R13-C) | 12,777 labels / 903,157 B / 5,187 resources (3,119 live / 2,068 deferred) |
| Ownership states | 8,938 ROM_BASE_ONLY + 3,125 COMPILED_PENDING_MIGRATION (= 12,063) |
| Range index | 5,835 registered of 8,192 (4,518 visual + 1,301 audio + 16 text arenas) |
| Pack entry caps | `GEN3_PACK_MAX_ENTRIES` 16,384 (`resource_pack.h:159`); merged import records 16,384 (`emerald_resource_import.c:61`) |
| Logical registry | `HOST_LOGICAL_ADDRESS_CAPACITY` 197 / `HOST_LOGICAL_RANGE_CAPACITY` 16 — audio-seam only |

---

## 1. Exact family inventory (pinned)

### 1.1 The legacy content.pak hydrate set (second identity — absorbed here)

Built by `src/platform/desktop_game_content.c` (`sCanonicalEntries[]` :61–77,
STATIC_ASSERT == 15 at :100) from the user's verified ROM at import
(`Platform_GameContentImport` :847; SHA-1 gated against `rom.sha1` →
`EMERALD_EXPECTED_SHA1`); installed as `<storage>/games/emerald/content.pak`
+ `manifest.json` (EMRLDATA format, 64 B header + 15×16 B entries + payload
at offset 304; total 323,391 B on disk, 323,087 B payload).

Every range was re-verified 1:1 against the qualified ELF:

| # | Entry | ROM offset | ROM wire bytes | ELF symbol (reference) | Hydrates (native) | Native bytes | Transform |
|---|---|---|---|---|---|---|---|
| 1 | species names | 0x3185C8 | 412×11 = 4,532 | `gSpeciesNames` 4,532 @0x083185C8 | `gSpeciesNames[412][11]` HOST_DATA | 4,532 | raw copy |
| 2 | move names | 0x31977C | 355×13 = 4,615 | `gMoveNames` 4,615 @0x0831977C | `gMoveNames[355][13]` HOST_DATA | 4,615 | raw copy |
| 3 | battle moves | 0x31C898 | 355×12 = 4,260 | `gBattleMoves` 4,260 @0x0831C898 | `gBattleMoves[355]` HOST_DATA | 3,195 (355×9) | **12 B padded wire → 9 B packed struct** |
| 4 | experience tables | 0x31F72C | 8×101×4 = 3,232 | `gExperienceTables` 3,232 @0x0831F72C | `gExperienceTables[8][101]` HOST_DATA | 3,232 | LE32 fixup |
| 5 | species info | 0x3203CC | 412×28 = 11,536 | `gBaseStats` 11,536 @0x083203CC | `gSpeciesInfo[412]` HOST_DATA | 10,712 (412×26) | **28 B padded wire → 26 B packed struct** |
| 6–15 | ten fonts | 0x62BAE4…0x66C8E4 | 8×32,768 + 2×16,384 = 294,912 | `gFont{SmallNarrow,Small,Narrow,Short,Normal}LatinGlyphs`, `gFont{Small,Normal}JapaneseGlyphs`, `gFont{FRLGMale,FRLGFemale,Short}JapaneseGlyphs` (sizes exact) | same-named `ALIGNED(4) HOST_DATA` u16 arrays, `src/fonts.c` | 294,912 | LE16 word load |

**The decisive layout fact (recomputed, corrects R13-A §5.1):** the vanilla
ROM rows are 4-byte-padded — BaseStats **28 B/row**, BattleMove **12 B/row**,
Evolution **8 B/entry**, ContestMove **8 B/row**, ContestEffect **4 B/row** —
while the recomp's native structs are packed (26 / 9 / 6 / 7 / 3 B). The
existing hydrator already performs the padded→packed conversion
(`HydrateEntry` :517–622, incl. EV-yield bitfield unpack and
bodyColor/noFlip bit split). Canonical R13-D payloads are the **exact ROM
slices (padded wire)**; the conversion is seam-owned, exactly as today, so
byte-parity stays `ROM slice == pack payload` and the transform has a live
oracle (current hydrated arrays).

Font facts (agent-verified):

- The ten font ranges carry **glyph bitmaps only** (raw 4bpp glyph wire:
  256 glyphs × 128 B Latin / half- and full-width Japanese layouts). The
  eight width tables (`gFont*GlyphWidths`, `src/fonts.c:8…296`, 512×u8 each)
  are compiled on every target and are NOT content.pak payload.
- Compiled font residue NOT in content.pak: `sFontBoldJapaneseGlyphs`
  (`INCBIN graphics/fonts/bold.hwjpnfont`, `src/text.c:265`), `sFont_Braille`
  (`src/braille.c:16`), keypad/menu-cursor tiles — all original payload in
  compiled form. Classification: §8.
- Consumers: `DecompressGlyph_{Small,Narrow,SmallNarrow,Short,Normal}`
  (`src/text.c:1711–1915`) index the hydrated arrays directly; `gFonts` /
  `sFontInfos` (`text.c:74,147–249`) are engine routing, unchanged.
- No font family exists anywhere in the resource system today (zero matches
  under `tools/gen3_resources/` and `resources/`). Fonts are not covered by
  R8/R9 graphics ownership.

### 1.2 Compiled species tables (native `.rodata`, unguarded — linked today)

| Table | Source (recomp) | Native rows × B | Native bytes | ROM canonical bytes | Pointer-bearing | Consumers |
|---|---|---|---|---|---|---|
| `gEvolutionTable` | `src/data/pokemon/evolution.h:1` (incl. `pokemon.c:1400`) | 412×5×6 | 12,360 | 412×5×8 = **16,480** (@0x0832531C) | no | `pokemon.c` GetEvolutionTargetSpecies :5476–5586; `daycare.c:405`; `evolution_scene.c:551–576` |
| `gLevelUpLearnsets` | `src/data/pokemon/level_up_learnset_pointers.h:1` | 412×8 | 3,296 | 412×4 = 1,648 (@0x0832937C) | **yes** (native ptr table) | `pokemon.c` :2997–3038, :6291–6361; `apprentice.c:350,474` |
| s*LevelUpLearnset leaves | `src/data/pokemon/level_up_learnsets.h` | 411 arrays | 8,766 | 8,766 (u16 `(lvl<<9)\|move`, 0xFFFF term; verified in reference ELF) | no | via pointer table |
| `gTMHMLearnsets` | `src/data/pokemon/tmhm_learnsets.h:10` (anonymous union, `as_u32s[2]`) | 412×8 | 3,296 | 3,296 (@0x0831E898) | no | `pokemon.c:6232–6272` (`as_u32s[i] & mask`) |
| `sTutorLearnsets` | `src/data/pokemon/tutor_learnsets.h:37` | 412×4 | 1,648 | 1,648 (@0x08615048) | no | `party_menu.c:2067–2072`; `field_specials.c:3218,3231` |
| `gTutorMoves` | `src/data/pokemon/tutor_learnsets.h:1` | 30×2 | 60 | 60 (@0x0861500C) | no | `party_menu.c` GetTutorMove |
| `gEggMoves` | `src/data/pokemon/egg_moves.h:5` | flat u16 | 2,278 | 2,278 (@0x0832ADD8) — 165 species blocks + terminator | no | `daycare.c` GetEggMoves :615–637 (linear scan w/ `ARRAY_COUNT`) |
| `gSpeciesIdToCryId` | `src/data/pokemon/cry_ids.h:1` | 135×2 | 270 | 270 | no (R12 cry routing) | `pokemon.c:5673–5682` |
| `sSpeciesToNationalPokedexNum` | `src/pokemon.c:524` | 411×2 | 822 | 822 | no | `pokemon.c:5600,5655` |
| `gPokedexOrder_*` | `src/data/pokemon/pokedex_orders.h` | 411/386/386×2 | 2,366 | 822/772/772 | no | `pokedex.c:2246–2302` |

`gFormSpeciesIdTable` **does not exist in this port** (zero symbols; the
R13-A mention is stale). `safariZoneFleeRate` is hydrated but has **no
consumer** in `src/` (dead field — kept for wire fidelity anyway).

### 1.3 Compiled move/contest tables

| Table | Source | Native bytes | ROM canonical bytes | Pointer-bearing | Consumers |
|---|---|---|---|---|---|
| `gBattleMoves` | hydrated (1.1 #3); GBA form `src/data/battle_moves.h:2` (guarded out on native) | 3,195 | 4,260 | no | **193 sites / 16 files** — `battle_script_commands.c` (78), `battle_ai_script_commands.c` (21), `pokemon.c` (13), `battle_tv.c` (8), `battle_main.c` (8), `pokemon_summary_screen.c` (7), `item_menu.c` (6), `battle_util.c` (4 incl. the :285 seam), 8 more |
| `gMoveNames` | hydrated (1.1 #2) | 4,615 | 4,615 | no | **78 sites** — direct `StringCopy(dst, gMoveNames[move])`; no GetMoveName accessor exists |
| `gMoveDescriptionPointers` | R13-C skeleton (GBA form `src/data/text/move_descriptions.h:2137`, `#ifndef NATIVE_LINUX`; native `text_skeleton_arrays.generated.c:1015`) | 354×8 | n/a (text rows in pack) | text ptrs → R13-C arena | `pokemon_summary_screen.c:3642`; `menu_specialized.c:818` |
| `gContestMoves` | `src/data/contest_moves.h:1` (incl. `contest_effect.c:58`) | 355×7 = 2,485 | 355×8 = **2,840** (@0x0858C2B4) | no (combo IDs embedded) | `contest.c` (11 sites), `contest_ai.c` (11), `move_relearner.c`, `menu_specialized.c`, `contest_effect.c` |
| `gContestEffects` | `contest_moves.h:2837` | 48×3 = 144 | 48×4 = **192** (@0x0858CDCC) | no | `contest.c`, `contest_ai.c`, `move_relearner.c` |
| `gContestEffectFuncs` | `contest_moves.h:3198` | 48×8 = 384 | (GBA: 192 = 48×4 code ptrs) | **fn pointers** — `ContestEffect_*` handlers, dispatched `contest.c:4477` | engine — stays compiled |
| `gComboStarterLookupTable` | `contest_moves.h:3131` | 64×1 (bool8) | 63 (@0x0858…; 63 entries) | no | `contest_effect.c:75,431` |
| Contest text tables | R13-C skeletons (`gContestEffectDescriptionPointers[48]`, `gContestMoveTypeTextPointers[5]`) | — | — | text ptrs → R13-C arena | `contest.c:3245`; `menu_specialized.c:849,852` |

Move-effect routing (verified): `gBattleScriptsForMoveEffects` —
`data/battle_scripts_1.s:20`, **214 `.int` GbaAddr entries**; consumed as
`HostResolveGbaAddr(gBattleScriptsForMoveEffects[gBattleMoves[move].effect])`
at `battle_util.c:285` and `battle_script_commands.c:4494,5770,6685,6693,
7940,9135`. `gBattleAnims_Moves` — `data/battle_anim_scripts.s:18`, **356
entries** (correction: R13-A §3.5 said 365). Both are already
GBA-logical-form (`const GbaAddr[]`) and resolve through `HostResolveGbaAddr`
(`host_memory.c:436`).

### 1.4 Compiled item tables

| Table | Source | Native bytes | ROM canonical bytes | Pointer-bearing |
|---|---|---|---|---|
| `gItems` | `src/data/items.h:1` (incl. `src/item.c:24`; **unguarded — compiled on native**) | 377×72 = **27,144** | 377×44 = **16,588** (@0x085839A0) | yes — embedded name[14] text + description GBA text ptr + `fieldUseFunc`/`battleUseFunc` GBA code ptrs |
| `gItemEffectTable` + leaves | `src/data/pokemon/item_effects.h:389` | 206×8 = 1,648 + ~145 u8 leaves | GBA: 163×4 = 652 (@0x0831E58C) | ptr table → data leaves |
| `gItemIconTable` | `src/data/item_icon_table.h:1` | 378×2×8 = 6,048 | 378×2×4 = 3,024 (@0x08614410) | gfx ptrs (icon leaves, 42,888 B LZ — R13-A rank 3) |

Native `struct Item` (`include/item.h:10`): name[14], itemId u16, price u16,
holdEffect u8, holdEffectParam u8, description u8* (offset 24), importance,
registrability, pocket, type, fieldUseFunc (40), battleUsage, battleUseFunc
(56), secondaryId — **72 B**. GBA wire row: same fields with 4-byte
pointers, **44 B**. Consumers: `gItems[` appears **only** inside the 15 thin
accessors `src/item.c:874–944` (`GetItemName`/`GetItemPrice`/…/
`GetItemSecondaryId`) — the cleanest seam in this stage.

Callback census (27 distinct functions): 21 field-use
(`ItemUseOutOfBattle_CannotUse` ×213 rows, `_TMHM` ×58, `_Medicine` ×38,
`_Mail` ×12, `_EvolutionStone` ×12, `_ReduceEV` ×6, `_PPRecovery` ×5, `_Rod`
×3, `_Repel` ×3, `_PPUp` ×2, `_BlackWhiteFlute` ×2, `_Bike` ×2, and 9 ×1) +
6 battle-use (`ItemUseInBattle_Medicine` ×35, `_PokeBall` ×12,
`_StatIncrease` ×7, `_PPRecovery` ×5, `_Escape` ×2, `_EnigmaBerry` ×1).

### 1.5 Revision-delta findings (gate before any cutover)

1. **`gItemEffectTable` family is fork-diverged.** Recomp: 206 rows incl.
   inserted `gItemEffect_EvolutionItem` rows (Kings Rock / Deep Sea Tooth /
   Deep Sea Scale / Metal Coat) and 145 leaf symbols; reference/vanilla: 163
   rows (`ITEM_POTION…LAST_BERRY_INDEX`, 138 leaves), no EvolutionItem rows.
   The recomp tree modified this table (early fork commits, e.g. `b7a74b43f`)
   — it is NOT a vanilla ROM slice. **Excluded from R13-D migration**;
   classified revision-delta debt (same class as the R13-A §5.5 wild
   encounter flags). Stays compiled, documented per-family exemption.
2. **Audit correction:** `gBattleAnims_Moves` = 356 entries, not 365
   (R13-A §3.5).
3. **Struct padding:** R13-A §5.1 listed hydrated rows as the canonical
   volume; the canonical ROM wire is padded (§1.1): species 11,536 (not
   10,712), battle moves 4,260 (not 3,195). The 10,712/3,195 figures are
   native in-memory sizes and remain correct for the arrays.
4. **`gComboStarterLookupTable`:** the recomp source carries 64 bool8
   initializers; the qualified reference ELF object is 63 bytes. A 1-byte
   divergence candidate — the §1.5.5 parity gate resolves which side is
   authoritative (canonical payload follows the ROM; a fork behavior change
   must be documented, never silently preserved).
5. **Compile-vs-vanilla parity obligation.** For every family still compiled
   on native (evolution, learnsets, TMHM, tutor, egg moves, contest, items),
   the generator must prove `recomp-compiled bytes (transformed where
   applicable) == vanilla ROM slice` before cutover. The recomp vendors a
   newer pret revision than the reference tree (R13-C AC-3 precedent: 21
   renamed text labels). Any divergence is a STOP-level revision-delta flag,
   resolved per family (vanilla authoritative for pack identity; a fork
   behavior change must be re-expressed deliberately, never silently).

---

## 2. Resource granularity and identity (pinned)

**Decision: per-entity, per-aspect, slice-pure.** One resource = one exact
ROM slice. This is the R13-B/C invariant (canonical payload == ELF bytes ==
ROM slice; per-record `romOffset`/size provenance; disjointness proof;
trivial byte-scan isolation) applied to structured rows. Entity-level
identity gives Tallgrass the `override species/charizard` mod surface the
brief requires; aspect-level sub-keys keep the separate ROM tables separate
(an evolution edit never touches the base-stats slice). Table-level identity
was rejected for the big three (fails the single-entity override contract);
composed multi-slice records were rejected (breaks slice purity, provenance,
and the disjointness proof).

Exact key vocabulary (IDs derived from the generator's enum-position
bindings — `SPECIES_BULBASAUR` → `bulbasaur`, `MOVE_POUND` → `pound`,
`ITEM_LEFTOVERS` → `leftovers`, `GROWTH_MEDIUM_FAST` → `medium-fast` — the
same derivation R13-C uses for `emerald:text/pokedex/<species>`; M0/M1 key =
`SHA-256("gen3-resource-id-v1\0" + exact canonical-name bytes)` as always):

| Key | Count | Payload | Schema |
|---|---|---|---|
| `emerald:data/species/<species>` | 412 | 28 B base-stats wire row | species-base |
| `emerald:data/species/<species>/name` | 412 | 11 B fixed-width charmap row | species-name |
| `emerald:data/species/<species>/evolutions` | 412 | 5×8 B wire entries | species-evolutions |
| `emerald:data/species/<species>/levelup` | 411 | packed u16 stream, 0xFFFF-term | species-levelup |
| `emerald:data/species/<species>/tmhm` | 412 | 8 B bitfield | species-tmhm |
| `emerald:data/species/<species>/tutor` | 412 | 4 B bitmask | species-tutor |
| `emerald:data/species/<species>/egg-moves` | 165 | block incl. `0x4000\|species` header (terminator rides the last block) | species-egg-moves |
| `emerald:data/move/<move>` | 355 | 12 B battle wire row | move-battle |
| `emerald:data/move/<move>/name` | 355 | 13 B fixed-width charmap row | move-name |
| `emerald:data/move/<move>/contest` | 355 | 8 B contest wire row | move-contest |
| `emerald:data/item/<item>` | 377 | 44 B GBA wire row | item |
| `emerald:data/growth-rate/<rate>` | 8 | 101×4 B curve | growth-rate |
| `emerald:data/tutor/moves` | 1 | 30×2 B | tutor-moves |
| `emerald:data/contest/effects` | 1 | 48×4 B | contest-effects |
| `emerald:data/contest/combo-starters` | 1 | 63 B | contest-combo-starters |
| `emerald:font/<font>` | 10 | glyph wire (32,768 or 16,384 B) | font schema 1 |
| **Total new resources** | **4,099** | **375,298 B canonical** | |

Species subtotal 2,636 resources / 48,536 B; moves 1,065 / 11,715 B; items
377 / 16,588 B; shared tables 11 / 3,547 B; fonts 10 / 294,912 B.

**Pack arithmetic:** 12,063 + 4,099 = **16,162 entries** — inside the
16,384 cap with 222 entries of headroom. **No cap raise in R13-D.** R13-E
(≈3,500 trainer/encounter/frontier/pokedex resources) provably exceeds the
cap and raises it then (documented arithmetic at that stage — the R13-C
precedent).

Names as structured rows (not TEXT): `gSpeciesNames`/`gMoveNames` are
fixed-width index-addressed rows (10/12 chars + 0xFF in 11/13 B), not
0xFF-terminated label strings; R13-C excluded them for exactly this reason
(AC-1 §3 "structured 2-D name tables, NOT text strings"). They migrate here
as STRUCTURED_DATA rows. Item names are row-embedded (`Item.name[14]`) and
travel inside the item resource — no separate identity.

---

## 3. Canonical vs transformed classification (settled per family)

Classes per the brief: A LEAF_BYTES, B GBA_POINTER_GRAPH, C
TRANSFORMED_STRUCTURAL, D INDEX_ROUTING, E ENGINE_CONSTANT.

| Family | Class | Canonical pack form | Native publication | Notes |
|---|---|---|---|---|
| species base rows | **C** | exact 28 B ROM wire rows | 26 B `struct SpeciesInfo` rows (today's HydrateEntry transform, seam-owned) | no pointers in row |
| species/move names | **A** (structured rows) | exact 11/13 B rows | raw copy into the existing arrays | charmap bytes; charmap-grammar valid (0xFF-terminated within fixed width) |
| battle moves | **C** | exact 12 B wire rows | 9 B `struct BattleMove` rows | no pointers |
| contest moves / effects | **C** | 8 B / 4 B wire rows | 7 B / 3 B rows | bitfields opaque bytes |
| combo starter lookup | **A** | 63 B | raw copy | |
| experience tables | **A** | 8×404 B LE u32 curves | raw LE copy | hydrator's LE32 path retained |
| evolutions | **C** | 5×8 B padded entries | 5×6 B `struct Evolution` rows | padding stripped per entry |
| level-up learnset leaves | **A** | per-species u16 streams | arena leaves + rebuilt pointer table | |
| `gLevelUpLearnsets` pointer table | **B** | n/a (derived) | generated HOST_DATA pointer table filled with arena leaf addresses | the only pointer-graph publication in R13-D |
| TMHM learnsets | **A** | 8 B rows | raw copy; anonymous-union type (`as_u32s`) preserved | |
| tutor learnsets | **A** | 4 B rows | raw copy | |
| `gTutorMoves` | **A** | 60 B | raw copy | |
| egg moves | **A** | per-species blocks; concatenation + terminator == exact 2,278 B stream | raw copy into `gEggMoves` array (ARRAY_COUNT semantics preserved) | |
| `gItems` | **C + callback registry** | exact 44 B GBA rows (ROM bytes incl. the two GBA code-pointer fields and one GBA text-pointer field) | 72 B native rows: data fields copied; description → R13-C text arena pointer; use funcs → registry-resolved native pointers | **no native function pointer is ever stored in pack payloads** — the pack holds vanilla ROM bytes; the registry is compiled engine metadata (§6) |
| `gItemEffectTable` + leaves | **DEFERRED** | — | stays compiled | fork-diverged (§1.5) |
| `gItemIconTable` + icon gfx | **DEFERRED** | — | stays compiled | gfx-leaf family for the R13-B-remaining wave (R9 machinery); item icons are not ROM_BASE-owned yet |
| fonts | **A** | exact glyph wire slices | u16 array fills (today's LoadFont semantics) | type FONT, not tile-graphics |
| `gContestEffectFuncs`, ItemUse funcs | **E** | — | compiled | engine dispatch/callbacks |
| `gSpeciesIdToCryId`, `sSpeciesToNationalPokedexNum` | **D** | — | stay compiled | architecture §1.3 class C routing (270 + 822 B); revisit at R14 boundary |
| `gPokedexOrder_*` | **D → R13-E** | — | stay compiled in R13-D | pokedex family migrates with `gPokedexEntries` in R13-E |
| `gBattleScriptsForMoveEffects` (214), `gBattleAnims_Moves` (356) | **D** | — | stay compiled (already GbaAddr form) | battle/anim script targets migrate in R13-H; unaffected by move-data cutover |
| type chart (`gTypeEffectiveness` 336 B) | **E/B engine constant** | — | stays compiled | architecture §2 class B mechanics constant; future Tallgrass schema candidate, not R13-D |

Function-pointer rule, applied: canonical payloads never carry native
function pointers. GBA code-pointer fields appear only as byte-exact ROM
bytes inside item rows (provenance) and are **validated + mapped** at
publication by the generated registry (§6); `gContestEffectFuncs` and the
move-effect/anim GbaAddr tables remain compiled engine artifacts.

---

## 4. gSpeciesInfo strategy

Native consumption today (agent-verified): **144 direct-index sites across 23
files** (`gSpeciesInfo[species].field`; two `CALC_BASE_STATS` macro sites at
`pokemon.c:2816` / `battle_dome.c:2508` still direct-index). Every field is
read except `safariZoneFleeRate`. No accessor layer exists, and none is
introduced.

Publication model — **republish-over-hydration** (architecture §5, §12 risk
2 mitigation):

- `HOST_DATA struct SpeciesInfo gSpeciesInfo[NUM_SPECIES]` keeps its exact
  declaration, symbol, section, and size. The only change: the byte source
  moves from content.pak hydration to the R13-D seam, which fills the array
  from the 412 `emerald:data/species/<species>` pack resources (28→26
  transform, identical semantics to `HydrateEntry CONTENT_SPECIES_INFO`).
- Zero call-site edits. `sizeof`/array semantics untouched; all 144
  consumers compile and run unchanged.
- Names, exp tables follow the same model (`gSpeciesNames`,
  `gExperienceTables` arrays filled from their resources).
- Rows are pointer-free → no State-v5 surface, no relocation records.

Mod override semantics (Tallgrass): a MOD provider entry for
`emerald:data/species/charizard` (exact-size 28 B wire row, schema
species-base) wins resolution over ROM_BASE; the seam is built from the
**final resolved snapshot** (architecture §17), so the published array row
carries the mod's stats. Override scope is exactly one species; all other
rows are ROM_BASE. Same for `/name`, `/evolutions`, `/levelup`, `/tmhm`,
`/tutor`, `/egg-moves` — independent sub-resources, so a learnset hack never
requires re-supplying base stats. Invalid optional overrides fall back to
ROM_BASE with trace evidence (existing resolver contract; exact-size catalog
constraints are the documented later extension — the seam's composition pins
are the v1 gate).

One resource per species row (not grouped table): decided §2. Grouped-table
publication would preserve ROM contiguity but fails the per-species override
contract; per-row slicing is proven at scale by R13-C (4,824 per-label
slices with disjointness proofs inside shared sections).

---

## 5. gBattleMoves strategy

Native consumption: **193 sites / 16 files**, all direct-index; fields
effect/power/type/accuracy/pp/secondaryEffectChance/target/priority/flags
all read. Publication mirrors §4: `HOST_DATA struct BattleMove
gBattleMoves[MOVES_COUNT]` filled from 355 × 12 B wire rows (12→9
transform). Zero call-site edits.

- **Per-move identity:** `emerald:data/move/<move>` (12 B) +
  `emerald:data/move/<move>/name` (13 B) + `emerald:data/move/<move>/contest`
  (8 B). A mod overriding `emerald:data/move/flamethrower` changes
  power/type/accuracy/pp/effect-chance/target/priority/flags in one 12-byte
  entry.
- **Byte-exact vs transformed:** canonical = byte-exact padded ROM row;
  native row is the transformed 9 B struct (class C, §3).
- **Effect routing semantics:** `effect` is a **data ID** (0–213), not a
  code pointer. The engine resolves it through the compiled
  `gBattleScriptsForMoveEffects[214]` GbaAddr table +
  `HostResolveGbaAddr` (battle_util.c:285 pattern) to a battle script.
  **A mod overriding power/type/etc. never touches engine routing** — the
  effect ID stays inside the vanilla effect space; the routing table and its
  script targets are engine/script identity (scripts migrate in R13-H).
  Assigning an existing effect ID to a move is a data override; creating a
  new effect behavior requires an engine mechanic adapter (R13-H-era, out of
  R13-D).
- **Relationship to R13-H:** battle scripts (`BattleScript_*`) and anim
  scripts remain compiled/`script_data` until R13-G/H; the GbaAddr routing
  tables stay exactly as they are. `gBattleAnims_Moves` (356 — corrected
  count) is untouched.
- Move descriptions: already live R13-C skeleton (`gMoveDescriptionPointers`
  filled from `emerald:text/move/<symbol>` resources) — no R13-D action.

---

## 6. gItems strategy

The trickiest table, settled as: **canonical 44 B GBA row resources +
compiled callback registry keyed by semantic action ID + validated
description re-pointing.** This matches the actual tree (data fields, text
pointer, and two engine callbacks in one struct) and the brief's preferred
shape.

Native layout audit (72 B) vs GBA wire (44 B), field by field:

| Field | Class | Treatment |
|---|---|---|
| `name[14]` | canonical data (embedded charmap text) | copied verbatim |
| `itemId`, `price`, `holdEffect`, `holdEffectParam`, `importance`, `registrability`, `pocket`, `type`, `battleUsage`, `secondaryId` | canonical data | copied verbatim |
| `description` | text pointer | validated + re-pointed: the row's 4-byte GBA address must equal the bound `emerald:text/item/<desc-symbol>` label's `romOffset` (known per R13-C binding); fill = that label's arena pointer |
| `fieldUseFunc`, `battleUseFunc` | engine callbacks | resolved through the generated **item-use callback registry**: 27 entries mapping GBA symbol ↔ reference-ELF address ↔ semantic action ID ↔ native `ItemUseFunc`; the seam looks up each row's two wire addresses; any address outside the registry is a fail-closed publication error |

- **No native function pointers in pack payloads** — pack entries hold exact
  vanilla ROM bytes (GBA code addresses included, as provenance bytes, exactly
  as the ROM stores them). The registry is a generated compiled artifact
  (class E), emitted by the R13-D generator from the reference ELF +
  recomp symbol set; the semantic IDs (`item-use/out-of-battle/medicine`,
  …) are also emitted as inventory metadata for future mod tooling.
- **Does gItems need a transformed publication table like R12-C?** Yes —
  `gItems` becomes a generated HOST_DATA table (same symbol, 377 rows, 72 B
  rows) filled at publish, the R13-C skeleton pattern adapted to full struct
  rows. The 15 accessors (`item.c:874–944`) are the only `gItems[` sites and
  need zero edits; the GBA flavor keeps the `const` definition untouched.
- **Transform oracle:** at publication, phase-1 validates that the
  transformed native rows equal the current compiled `gItems` rows
  field-by-field (name bytes, data fields, description pointer targets,
  function identities). This is the cutover equivalence proof — and the
  revision-delta detector: any field mismatch (e.g. a newer-pret price
  change) halts cutover as a STOP-level flag (§1.5.5).
- Item effect leaves (`gItemEffectTable`) and item icons
  (`gItemIconTable` + 42,888 B icon gfx) are **out** (§1.5.1, §3): the
  former is fork-diverged debt; the latter is the R13-B-remaining gfx-leaf
  wave. Neither blocks item-row ownership.
- Shops/marts: no compiled inventories exist (script-driven `sMartInfo`
  EWRAM lists) — nothing to migrate.

---

## 7. content.pak absorption (retire entirely in R13-D)

Traced lifecycle (§1.1, agent-verified): `Platform_GameContentImport`
(sdl2.c `--import-rom` :1427; frontend data-setup `desktop_frontend.c:1159`;
settings re-import :926) builds the pak from the verified ROM;
`Platform_GameContentVerifyInstalled(TRUE)` is the **boot gate**
(`sdl2.c:1589,1604`; frontend :908) — it validates, hydrates all 15
families, then runs the NATIVE_LINUX block that registers the .rpack
runtime snapshot (`EmeraldResourceCompat_RegisterRuntimeSnapshot` +
`EmeraldResourceCompat_TryInitialize` + `NativeWorldNeighborhood_Init`,
`desktop_game_content.c:741–778`). The game cannot start without
content.pak today; the .rpack session is registered *inside* its verify
path. The dependency direction is already one-way (zero references to
content.pak inside `src/emerald/resources/`).

Per-family decision — every content.pak payload acquires ROM_BASE identity
in R13-D:

| content.pak family | Decision |
|---|---|
| species info / battle moves / species names / move names / exp tables | **absorbed now** — become STRUCTURED_DATA pack resources (§2); hydration replaced by seam publication |
| 10 fonts | **absorbed now** — `emerald:font/<font>` (type FONT) |
| content.pak file format / builder / manifest.json / `Platform_GameContent*` API surface | **removed entirely in R13-D** (not retained as cache) |

Rationale for full removal (over temporary cache): the goal is ONE
authoritative identity per datum; a retained pak keeps a second
authoritative byte source for the most-indexed tables in the game, the
exact debt the architecture (§22 risk register: "Old content.pak persists →
treat as migration debt and retire it") directs us to pay down. Every pak
byte is re-proven through the pack's three-way chain; there is no content
the pak can supply that the pack cannot.

Boot/lifecycle re-wiring (the loader lifecycle change the architecture
anticipated):

1. First-run / re-import: the frontend "Game Data Required" flow switches
   from `Platform_GameContentImport` (pak builder) to the existing,
   test-proven ROM→rpack importer (`EmeraldImport_ValidateRom` /
   `BuildPack` / `Install`, `emerald_resource_import.{c,h}` — today only
   exercised by tests) with the merged 16,162-record manifest set. One ROM
   import installs the single authoritative pack.
2. Boot verify: `sdl2.c:1589/1604` calls the new game-data verify —
   installed-pack validation + session registration (the code block
   currently inside `VerifyPaths` moves verbatim into the new path) — then
   the R13-D seam publishes gameplay tables + fonts. Order stays: session
   registration before gameplay publication, both before `AgbMain`.
3. Existing installs migrate by one re-import (frontend detects missing
   rpack → data-setup screen); leftover `content.pak`/`manifest.json` are
   inert and removed opportunistically (documented; never read again).
4. `desktop_game_content.c` shrinks to nothing (deleted with its header and
   the `EMRLDATA` format); `EMERALD_EXPECTED_SHA1` moves to the importer's
   ROM profile (already duplicated in `emerald_rom_profile.c`).

Failure semantics: a refused pack/session remains a REFUSED install
(R12-E contract); gameplay publication failure is REFUSE-class for the
cut-over families (their compiled definitions are guarded out — there is no
fallback to fall back to; §11 of R13-C set the precedent).

---

## 8. Fonts (precise classification)

- **Not covered by graphics ownership today** — no font family exists in
  the pack, catalogs, or any generator; R9 never touched fonts (they were
  "already ROM-backed through the old package").
- **content.pak duplication:** none against the rpack (fonts exist only in
  the pak); after §7, only in the pack.
- **Native host representation:** the hydrated `u16` glyph arrays ARE the
  host representation the renderer needs (`DecompressGlyph_*` index math in
  `text.c`); the seam fills them from pack payloads with today's exact LE16
  semantics. No second representation.
- **Classification:** original font wire, type `FONT` (pack code 8, already
  in the append-only enum), schema 1, representation `gba-bytes`. Fonts do
  NOT enter tile-graphics schemas/validators — **no second graphics
  migration is created** (architecture §6 row "Fonts | original font wire |
  hydrate/endian-convert where required", implemented literally).
- **Compiled residue kept:** the 8 width tables (engine-coupled render
  data, compiled on every target incl. GBA), `sFontBoldJapaneseGlyphs`,
  `sFont_Braille`, keypad/menu-cursor tiles. `sFont_Braille` is already
  assigned to the R13-B-remaining leaf wave (architecture §7); bold +
  keypad icons join that wave (documented here; both are small INCBIN leaves
  with reference-ELF provenance available). `gFonts`/`sFontInfos` routing
  stays engine (class E/D).

---

## 9. Names/descriptions interaction with R13-C

| Text family | R13-C state | R13-D action |
|---|---|---|
| Move descriptions (355, `emerald:text/move/<symbol>`) | **live** (skeleton `gMoveDescriptionPointers[354]`) | none — already ROM-backed |
| Ability / nature / contest-effect text | **live** (skeletons) | none |
| Item descriptions (310, `emerald:text/item/s<item>desc`, 15,101 B) | deferred, COMPILED_PENDING_MIGRATION — blocked on `gItems` owning the pointer field | **completed here**: gItems publication re-points descriptions to the item arena (§6); the 310 resources flip ROM_BASE_ONLY; compiled `s*Desc` arrays are guarded out. This is the explicit text-cutover completion the brief asks about. |
| Species names / move names | excluded from R13-C (structured fixed-width rows) | migrated as STRUCTURED_DATA rows (§2) — **not duplicated as text resources** |
| Item names | row-embedded in `gItems` | travel inside the item resource; no separate identity |
| Pokédex entries + categories | deferred; blocked on `gPokedexEntries` | **R13-E** (table owns the pointers); R13-D must not broaden into pokedex rows — `gPokedexOrder_*` stays compiled here and migrates with the pokedex family |
| Easy-chat / berry / cable-club deferred text | deferred on tables/blobs | R13-E / leaf wave, unchanged |

The text seam's 16 arenas (incl. the `item` arena) are already allocated
and range-registered; R13-D changes only which bytes serve item-description
reads (compiled → arena), with zero seam changes on the text side.

---

## 10. Consumer seams (classified per family)

| Family | Live consumers | Classification | Mechanism | Consumer edits |
|---|---|---|---|---|
| gSpeciesInfo (144 sites/23 files) | direct index | LIVE_CAN_CUT_OVER_DIRECTLY | republish-over-hydration (§4) | **zero** |
| gBattleMoves (193/16) | direct index | LIVE_CAN_CUT_OVER_DIRECTLY | republish (§5) | **zero** |
| gSpeciesNames / gMoveNames (78+ sites) | direct `StringCopy` | LIVE_CAN_CUT_OVER_DIRECTLY | array fills | **zero** |
| gExperienceTables (~20 sites) | direct `[growthRate][level]` | LIVE_CAN_CUT_OVER_DIRECTLY | array fills | **zero** |
| gEvolutionTable (pokemon/daycare/evolution_scene) | direct `[species][i]` | NEEDS_TRANSFORMED_TABLE | 8→6 row transform into the same array | **zero** |
| gLevelUpLearnsets + 411 leaves (pokemon/apprentice) | pointer-table deref | NEEDS_TRANSFORMED_TABLE | arena leaves + generated pointer table filled at publish | **zero** |
| gTMHMLearnsets (`as_u32s`) / sTutorLearnsets / gTutorMoves / gEggMoves / contest data | direct index | LIVE_CAN_CUT_OVER_DIRECTLY | raw row copies (union/array types preserved; `ARRAY_COUNT(gEggMoves)` intact) | **zero** |
| gItems (15 accessors only) | accessor layer | NEEDS_CALLBACK_REGISTRY (+ transformed table + text re-point) | §6 | **zero** |
| Fonts (DecompressGlyph_*) | direct glyph-array index | LIVE_CAN_CUT_OVER_DIRECTLY | array fills from pack | **zero** |
| gItemEffectTable / gItemIconTable | party menu / AI / icons | DEFER_TO_LATER_STAGE | — (§1.5.1, §3) | — |
| Routing tables (cry, dex-num) | pokemon.c | DEFER_TO_LATER_STAGE (stay compiled, class D) | — | — |
| Move-effect/anim GbaAddr tables | battle engine | DEFER_TO_LATER_STAGE (R13-H scripts) | already HostResolveGbaAddr form | — |

Generated publication tables/accessors are used everywhere; **no mass
call-site edits anywhere** — the stage's entire consumer surface is
symbol-identity preservation.

---

## 11. State-v5

**No format change. No new sidecar record class. No cap change.**

Serialized pointer audit for R13-D data:

- Species/move/item/exp/learnset/evolution arenas: **zero serialized
  pointer surface** — all consumers are transient (R13-A §6 re-confirmed:
  daycare copies u16 move IDs; parties/store mons by species/move IDs; no
  row pointer is cached in any EWRAM/IWRAM/COMMON/GAME_DATA slice; bag and
  menu description reads pass the pointer straight to the text printer).
- The one interior-pointer class remains `TextPrinter.currentChar`: while an
  item description prints, `currentChar` points into the **item text arena**
  (registered by R13-C; deferred labels served from compiled bytes until
  R13-D re-points `gItems.description`, after which the pointer lands in the
  arena). R13-C's walker routing (range hit → sidecar record keyed
  `emerald:text/arena/item`, offset-in-range; opaque fallback for compiled
  text) already covers this — R13-D adds the scenario to the cross-restart
  suite (creator prints an item description → save → fresh-process load →
  resumed print), not machinery.
- Font glyph arrays, callback registry, published struct arrays: never
  pointer-referenced from serialized state (renderer/engine reads them
  directly).

Range registrations (the only State-v5 work): one range per published
family arena, role COMPAT_OBJECT — species (base/name/evolutions/tmhm/tutor
arena), levelup-leaf arena, egg-moves arena, move arena, contest arena,
item arena, growth/tutor/effects arena, font arenas (≤12 ranges total).
Registration is cheap insurance: any unforeseen serialized pointer into
these arenas becomes a capturable/restorable sidecar record instead of an
unmanaged-pointer capture failure.

**Capacity arithmetic (corrects the stale R13-C §9 projection):** the
current index holds **5,835** ranges (4,518 visual + 1,301 audio + 16 text),
not the 2,040 figure the R13-C plan inherited from pre-R11 docs. Headroom
is 2,357. Per-row ranges for species/move/item rows (the old "~4,000 row
ranges after R13-D/E" projection) would **bust the cap and are rejected** —
nothing serializes into these arenas, so per-row ranges buy nothing. R13-D
adds ≤12 → ≈5,847 of 8,192. R13-E must use the same arena discipline.

---

## 12. Isolation

Proof chain per family after cutover: `source/ELF == ROM == pack ==
published table`, then **compiled original payload absent** from fresh `-B`
release + DINFO (both flavors), via the existing runner grammar — symbol
absence, table-shape proof, row-level byte scans where discriminating,
whole-table scans, precise documented exemptions. **No blanket data-table
exemption.**

| Family | Primary proof | Byte-scan treatment |
|---|---|---|
| 5 hydrated tables + fonts (absorption) | hydration path removed: `desktop_game_content` payload symbols absent; pack three-way chain per record; publication equality vs the transform oracle | fonts (16–32 KB each) trivially scannable; table rows covered by the transform-oracle equality + ROM-slice sha records |
| gItems | symbol absence of the compiled `gItems` object (`.rodata` nm sweep) + row-level scan: 377×44 B wire rows are distinctive (name+price bytes); sha-keyed whole-table scan as backstop | 44 B rows ≥ scan floor; per-row sha records |
| evolution rows | compiled `gEvolutionTable` symbol absence; whole-table sha (native 12,360 vs ROM 16,480 shapes both absent) | 8 B entries below row-scan floor → table-shape + symbol proof (documented per-table class, not blanket) |
| learnset leaves + pointer table | 411 leaf symbols absent (LOCAL — full `nm --defined-only` sweep, R13-B movement pattern) + pointer-table symbol absence | leaves are short (median ~20 B): table-level sha over the leaf zone + symbol proof; sub-1 KB rows = existing NOTE class |
| TMHM/tutor/egg/contest numeric tables | symbol absence + whole-table sha scans | rows 3–8 B: table-shape proof class (documented per table) |
| item descriptions | R13-C §12 grammar applies: 310 symbol absences + payload scans ≥16 B; compiled `s*Desc` objects absent | |
| gItemEffectTable / icons | **documented compiled exemption** (revision-delta debt / gfx-leaf wave) — individually named, reasons recorded in ownership TOML | never a section-wide exemption |
| routing tables | documented class-D exemption (270 + 822 B; architecture §1.3) | |

Additions to the isolation runner: gameplay-data ownership file(s) wired
into `tests/run_emerald_native_asset_isolation.sh` (the runner currently
consumes nine ownership files and has no text-family wiring either — the
text family's proof lives in the R13-C battery; R13-D wires its own files
and records the text-wiring debt). Disjointness proof for all new ROM
slices (R13-B E8 pattern).

---

## 13. ROM-hack import model (Tallgrass)

| Hack change | R13-D resource(s) | Class |
|---|---|---|
| Changed Charizard stats | override `emerald:data/species/charizard` (28 B wire row) | direct data override |
| Changed Charizard name | `emerald:data/species/charizard/name` | direct |
| Changed evolution method (vanilla method space) | `emerald:data/species/<s>/evolutions` | direct |
| Changed level-up move | `emerald:data/species/<s>/levelup` | direct |
| Expanded TM compatibility | `emerald:data/species/<s>/tmhm` | direct |
| Changed tutor compatibility | `emerald:data/species/<s>/tutor` | direct |
| Changed egg moves | `emerald:data/species/<s>/egg-moves` | direct |
| Changed Flamethrower power/type/accuracy/PP | `emerald:data/move/flamethrower` | direct |
| Changed move's contest data | `emerald:data/move/<m>/contest` | direct |
| Changed Rare Candy price | `emerald:data/item/rare-candy` | direct |
| Changed item description | `emerald:text/item/scandyraredesc` (R13-C identity, live after R13-D) | direct |
| Changed growth curve | `emerald:data/growth-rate/<rate>` | direct |
| New item-use behavior (new fieldUseFunc) | registry extension — **requires native mechanic adapter** (new compiled callback + registry entry; not data-only) | mechanic adapter |
| New evolution method beyond vanilla enum | engine + schema extension | beyond vanilla schema |
| New move effect behavior | engine (effect handler + battle script) — R13-H-era | mechanic adapter |
| Traditional hack's rebuilt tables | the hack's patched ROM re-imports through the same generator pipeline: per-row resources diff naturally (one changed row = one override) | direct |

Identity design consequence (validated): because keys are semantic
per-entity names over exact row slices, a conventional Emerald ROM hack maps
onto MOD overrides row-for-row with zero table-level replacement.

---

## 14. Expansion / headroom (do now: nothing; preserve: identity)

Vanilla-fixed in R13-D (explicit stage contract):

- Counts are compile-time constants and seam composition pins:
  `NUM_SPECIES` 412 (59 use sites), `MOVES_COUNT` 355 (35), `ITEMS_COUNT`
  377 (71), `EVOS_PER_MON` 5, `TUTOR_MOVE_COUNT` 30, 8 growth rates. The
  published arrays keep static sizes (`ARRAY_COUNT(gEggMoves)` etc.
  preserved); the seam refuses any pack whose composition differs.
- Row wire formats are vanilla formats (28/12/8/8/4/44 B schemas).

Not hard-coded by identity:

- Keys are **name-based, count-free** (`emerald:data/species/<name>`,
  `/levelup`, …). Nothing in a key encodes an index or a count; a future
  Tallgrass registry that adds `species/<new-mon>` entries needs no rename
  of any vanilla resource and no key-scheme change.
- Schemas describe row *shape*, not table *length*; per-entity resources
  mean expansion is additive (new entries), never a table-format break.
- The seam's count pins are the single point a future registry generalizes
  (pins → registry-driven counts), so expansion is a seam change, not an
  identity change.

Expansion is NOT implemented, designed-around, or promised here — only not
precluded.

---

## 15. Subdivision decision: split R13-D into D1 + D2

**Split.** Not because the family count is large, but because the stage
contains two orthogonal, independently risky axes:

- **R13-D1 — species/moves/shared tables/fonts + content.pak absorption.**
  The systemic-risk item is the **loader lifecycle change**: retiring the
  boot-gating second package, rewiring first-run import to the rpack
  importer, moving session registration out of `VerifyPaths`. Every table
  in D1 is a republish-over-hydration or raw-fill with a live oracle — low
  per-table risk, high integration risk.
- **R13-D2 — gItems transformed publication + callback registry + item text
  cutover.** The hardest single table (transform + text re-point + 27-entry
  callback registry + the revision-delta oracle), but entirely self-
  contained: the accessor seam exists, the boot path is untouched, and it
  depends only on D1's seam/vocabulary and the already-packed R13-C item
  text.

Each sub-stage lands with its own full battery + isolation + fresh `-B`
builds + report, giving two clean stop conditions instead of one entangled
one. D2 also de-risks R13-E (trainers carry the same "pointer table +
text-pointer rows" pattern at scale).

---

## 16. Implementation order (Flash order, derived from the actual tree)

### R13-D1

1. **Gameplay-family generator** (`tools/gen3_resources/gameplay_family/`):
   enum-position bindings (species/move/item/growth-rate ID → name), row
   slicing from the qualified reference ELF + ROM, three-way
   ELF==ROM==artifact byte proof per record, charmap validation for name
   rows, the §1.5.5 **compile-vs-vanilla parity gate** for every
   still-compiled family (evolution/learnsets/TMHM/tutor/egg/contest/items
   data fields), revision-delta flags surfaced fail-closed; outputs:
   inventory/catalog/bindings/ownership/consumers TOML + row artifacts +
   generated seam tables; `--check` determinism (R13-B E0 pattern).
2. **Vocabulary extension**: append `GEN3_PACK_TYPE_STRUCTURED_DATA = 16`
   (+ host enum), the 15 schema codes (§2), `TypeCompatibleWithRepresentation`
   pair (structured-data, gba-bytes), the (font, gba-bytes) pair, importer
   type-name parsing; schema discipline = manifest validator fail-closed.
3. **Pack**: merged 16,162-entry production pack (9 existing sources +
   gameplay + font manifests/catalogs); `gen3-pack-build --check`
   byte-for-byte proof; provenance pins (ROM SHA-1/256 per record); cap
   check (16,162 ≤ 16,384 — no raise, documented).
4. **Publication seam** `src/emerald/resources/emerald_gameplay_compat.{c,h}`
   (D1 scope): transactional phase-1 validation (composition pins incl.
   412/411/355/165/8/30, M0/M1 resolution, ROM_BASE winner, size + byte
   equality vs session views, disjointness) → phase-2 fills (species/move/
   name/exp arrays, evolution 8→6 transform, levelup leaf arena + generated
   pointer table, TMHM/tutor/egg/contest raw fills, font u16 fills) →
   atomic publish; REFUSE-class with full rollback; named diagnostics
   (128-char-buffer lesson applied); arena range registration (≤12).
   Wired after the text seam in `emerald_runtime_loader.c`.
5. **Live cutover**: guard out the compiled D1 definitions under
   `NATIVE_LINUX`/`DESKTOP_EXTERNAL_GAME_CONTENT` as appropriate (evolution,
   learnsets incl. leaves, TMHM, tutor, egg moves, contest numeric tables);
   GBA flavor untouched; native link is the completeness proof (R13-C §6.4
   pattern; `text_native_exports.c` precedent if statics surface).
6. **content.pak absorption**: wire `EmeraldImport_ValidateRom/BuildPack/
   Install` into the frontend data-setup + `--import-rom`; move session
   registration + neighborhood init to the new verify path; delete
   `desktop_game_content.c` + header + EMRLDATA format + frontend pak
   references; storage migration note (one re-import).
7. **State-v5**: arena ranges live (≤12); cross-restart suite gains the
   item-description-print scenario (prepared for D2 cutover) + gameplay
   arena pointer-capture negative tests.
8. **Isolation + battery**: gameplay ownership files in the isolation
   runner; **fix the two stale pins** found during this audit
   (`tests/emerald_audio_compat_test.c:942` still 6,876; `run_r13b_leaf.sh`
   still the 8-source/6,876 recipe — both predate the R13-C pack and
   slipped because neither is in the R13-C battery); full 25+ suite battery
   re-pinned to 16,162; fresh `-B` release + DINFO with size deltas; D1
   report.

### R13-D2

9. **Item callback registry generator**: 27 entries (reference-ELF address ↔
   pret symbol ↔ semantic action ID ↔ native function), emitted as compiled
   registry TU + binding metadata; fail-closed on any unknown row address.
10. **gItems transformed publication**: `gItems` becomes the generated
    HOST_DATA table filled from 377 × 44 B resources (transform §6: data
    copy + description re-point validated against R13-C label romOffsets +
    registry resolution); phase-1 oracle equality vs the current compiled
    rows; STOP on any revision delta (§1.5.5).
11. **Item text cutover**: 310 `emerald:text/item/*` resources flip
    ROM_BASE_ONLY; compiled `s*Desc` arrays guarded out (per-label
    `#ifndef NATIVE_LINUX`, R13-C pattern); ownership TOML updated.
12. **D2 isolation + battery + fresh builds + report**; guard out compiled
    `gItems`; re-run the full battery and both runners.

---

## 17. Projected totals

| Quantity | Value |
|---|---|
| Species resources / bytes | 2,636 / 48,536 B (base 412×28; name 412×11; evolutions 412×40 ROM-wire; levelup 411 leaves 8,766; tmhm 412×8; tutor 412×4; egg 165 blocks ⊂ 2,278) |
| Move resources / bytes | 1,065 / 11,715 B (battle 355×12; name 355×13; contest 355×8) |
| Item resources / bytes | 377 / 16,588 B (44 B GBA rows) |
| Evolutions / learnsets / compat | evolutions 412 rows; levelup 411 leaves; TMHM 412; tutor 412 (+30-move list); egg 165 blocks |
| Shared tables | growth rates 8×404 (3,232); tutor move list 60; contest effects 192; combo starters 63 — total 3,547 B |
| Fonts | 10 / 294,912 B |
| content.pak families absorbed | all 15 (5 gameplay + 10 fonts; 323,087 B payload) — pak format removed |
| New resources total | **4,099** (D1: 3,722; D2: 377) |
| Canonical bytes into pack | **375,298 B** |
| Pack entries | 12,063 → **16,162** (≤ 16,384; no cap raise) |
| Pack size | 12,100,064 → ≈ **13.3 MB** (payload 375 KB + TOC 656 KB + names ≈ 150 KB) |
| Compiled payload leaving native | ≈ **76.6 KB** (gItems 27,144; evolution 12,360; learnsets 12,062; TMHM 3,296; tutor 1,708; egg 2,278; contest numeric 2,693; item-desc text ≈ 15,101) |
| Release binary delta | ≈ **+0.2 to +0.9 MB net** (payload removal vs seam + generated inventory metadata; R13-B/C precedent: metadata grows the binary while payload ownership moves to the pack; packed-name-zone storage recommended to stay at the low end) |
| Range index | 5,835 → ≈ **5,847** of 8,192 (≤12 arena ranges; per-row ranges rejected) |
| Sidecar / State-v5 | no format change; no new record class; +1 cross-restart scenario |
| Complexity | D1 medium-high (generator + seam + boot rewiring): ≈ 2–3 weeks; D2 medium (transformed table + registry + text cutover): ≈ 1–2 weeks; each incl. full battery, isolation, fresh builds |
| Split | **yes — D1 (species/moves/shared/fonts + content.pak absorption) / D2 (gItems + callback registry + item text cutover)** (§15) |

STOP conditions (architecture §13, applied here): any migrated payload
still compiled without a documented structural exception; any live pointer
falling back to removed data; any PCM/state delta; unexpected range/hull/
count movement; startup failure with a valid pack; an uncrashable invalid
pack; a revision delta resolved silently; or any engine rewrite (struct
layout changes, VM changes) instead of relocation/republish.

**R13-D is safe to start on this plan. STOP here — no implementation, no
commit, no R13-E.**

---

## Implementation delta (R13-D1 — supersedes the plan's D1 numbers)

R13-D1 is **implemented and verified** (report:
`docs/R13D1_GAMEPLAY_DATA_CONTENTPAK_REPORT.md`). Facts that supersede the
plan above:

1. **Evolution family excluded from migration.** The compile-vs-vanilla parity
   gate proved the recomp deliberately replaced the 12 vanilla trade
   evolutions with item/level-40 evolutions (`evolution.h:43,45`). Per the
   brief's STOP rule, `gEvolutionTable` **stays compiled** (documented
   behavioral-divergence exemption). This is the proven correction to the
   3,722 pin.
2. **D1 inventory** = 3,310 resources / 342,230 B (was 3,722 / 358,710 B).
   The 412 evolution resources / 16,480 B are not emitted. Every parity-
   passing family ships.
3. **Pack** = 12,063 → **15,373 entries**, 13,102,112 B, SHA-256
   `d2f0e033…ddf013`, deterministic rebuild byte-identical. No cap raise.
   D1+D2 final projection = **15,750** (was 16,162).
4. **Content.pak fully retired** (EMRLDATA/sCanonicalEntries/HydrateEntry
   gone); boot registers the installed .rpack and the D1 seam publishes the
   game tables + fonts; `--import-rom` builds the .rpack via EmeraldImport.
5. **Vocabulary/gen type**: `GEN3_PACK_TYPE_STRUCTURED_DATA = 16` +
   `GEN3_RESOURCE_TYPE_STRUCTURED_DATA`, validator pairs enabled, type name
   wired in the importer + three test harness parsers.
6. **Transforms**: species 28→26, move 12→9, contest move 8→7, contest effect
   4→3 (seam-owned); names/tmhm/tutor/tutor-moves/egg/growth/combo raw;
   learnset leaves → arena + rebuilt pointer table; fonts LE16.
7. **Ranges**: 12 COMPAT_OBJECT arena ranges (levelup, egg, 10 fonts);
   range index 5,835 → 5,847/8,192.
8. **Gates**: `--verify-game-data` exit 0; loader battery 62,934 checks;
   trainer-compat 37,921; world-real 5,565; render-proof 3,628 — all pass.
   Release binary 22,176,784 → 22,368,088 B (+191,304 B).
9. D2 (gItems + callback registry + item text) is unchanged by D1: schema 11
   already permitted; the transform/index-map patterns extend directly.

**R13-D1 STOP.** No commit made. D2 and R13-E not started.
