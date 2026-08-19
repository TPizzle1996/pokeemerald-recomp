# R13-A — Emerald Remaining Content Audit

Scope: establish the exact remaining compiled original-game content surface —
text, scripts, map metadata, gameplay tables, and miscellaneous structured
data — its pointer graphs, native consumers, binary placement, and
save-state interaction, before any R13 migration work. This audit is
inventory-only. No production code was modified (verified: working tree
contains only the R12-G deliverables plus the pre-existing `android/SDL2`
submodule state; no source edits were made during this audit).

Companion: `docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md` (classifications,
vocabulary, dependency graph, stages, isolation approach).

**Status (2026-08-18):** R13-B (movement scripts + multiboot programs)
is complete — inventory re-verified (movement 1,055 resources / 7,428 B —
the 7,576 B figure above was 148 B high; see §3.1), payloads migrated to
the ROM_BASE pack, additive seam + failure matrix green, isolation
battery green. See `docs/R13B_LEAF_PAYLOAD_MIGRATION_REPORT.md`.

Audited binaries (both fresh `-B` builds of the R12-complete tree, tag
`checkpoint-r12g-complete`):

| Binary | Size | `.rodata` | `script_data` | `host_data` | `gba_ewram` |
|---|---|---|---|---|---|
| release (`pokeemerald-linux64`) | 20,952,576 B | 3,774,232 | 1,065,814 | 7,958,865 | 144,985 |
| DINFO | 33,214,128 B | 4,223,416 | 1,065,814 | 7,958,880 | 145,269 |

The payload sections (`script_data`, `host_data`, `gba_ewram`) are
flavor-invariant by construction (same linker script, same payload content);
only `.text`/`.rodata`/debug sections differ between flavors.

## 1. Whole-binary remaining-content audit

### 1.1 Output-section attribution (release, linker map `/tmp/r13-native.map`)

| Output section | Range | Size | Verdict |
|---|---|---|---|
| `.text` | 0x403940–0x7d7874 | 4,013,876 | engine code — stays compiled |
| `.rodata` | 0x7d8000–0xb71718 | 3,774,232 | const payload — the main R13 target |
| `.eh_frame*` | — | 555,268 | unwind metadata |
| `.data` | 0xbfb4a0–0xc12710 | 94,832 | hydrated content-pak tables + misc |
| `gba_common` | 0xc12710–0xc15d7d | 13,933 | serialized-slice live objects (state) |
| `gba_ewram` | 0xc15d80–0xc393d9 | 144,985 | EWRAM-slice live objects (state) |
| `host_data` | 0xc393e0–0x13d0531 | 7,958,865 | native renderer/compat scratch — engine, NOT payload |
| `script_data` | 0x13d0536–0x14d488c | 1,065,814 | script bytecode + asm text (writable) |
| `game_data` / `game_bss` / `.bss` | — | 64 / 287,792 / 2,959,208 | serialized state + engine bss |

`host_data` (7.96 MB) is almost entirely native engine scratch
(`sExpandedObjOut` 1,728,528; `sCompObjOut` 1,728,528; `sExpandedBgLayers`
691,200; `sHostPointers` 524,288; `sHostFunctions` 524,288; `sNativeOverworldFramebuffer`
345,600; …). It contains no original-game payload and is out of R13 scope.
`gba_ewram` is runtime state (`load_save.o` 55,108; `fieldmap.o` 20,544;
`decompress.o` 16,384; `sprite.o` 12,544; …), also out of R13 scope.

### 1.2 Largest remaining original-game payload contributors (ranked)

| Rank | Family | Section | Approx bytes | Class |
|---|---|---|---|---|
| 1 | Event/map scripts + asm text (event_scripts.o) | script_data | 975,626 | A — bytecode + text |
| 2 | Battle anim scripts | script_data | 63,811 | A |
| 3 | GBA graphics blobs in `graphics.o` (icons 430,080; still pics 363,816; battle-anim sprite gfx 110,312; item icons 42,888; battle-bg images 32,604; footprints 12,384; …) | .rodata | ≈1,040,000 | A — gfx leaves (R9 machinery applies) |
| 4 | Pokedex entries text (`pokedex.o`) | .rodata | 88,438 obj / 58,017 text | A |
| 5 | Frontier tables (`battle_tower.o`) | .rodata | 77,982 obj | A |
| 6 | Movement-action tables (`event_object_movement.o`) | .rodata | 68,576 obj | A (see §3) |
| 7 | Bard sound templates (`bard_music.o`) | .rodata | 65,362 obj | A |
| 8 | Battle transition gfx/tables (`battle_transition.o`) | .rodata | 64,928 obj | A — gfx leaves |
| 9 | Weather gfx (`field_weather.o` 49,856) + door anims (40,448) + trade gfx (51,766) | .rodata | ≈140,000 | A — gfx leaves |
| 10 | `gItems` + item descriptions (`item.o`) | .rodata | 46,777 obj / 42,245 data | A |
| 11 | `gTrainers` (`data.o`) | .rodata | 41,040 | A |
| 12 | Multiboot programs (Colosseum 163,840; ereader 25,024; berry glitch fix) | .rodata | ≈190,000 | A — leaf blobs |
| 13 | `strings.c` system strings | .rodata | 40,781 obj / 31,258 text | A |
| 14 | Easy chat words | .rodata | 38,240 obj / 7,101 text | A |
| 15 | Wild encounter tables (`wild_encounter.o`) | .rodata | 19,696 obj | A |
| 16 | Species learnset/evolution tables (pokemon.o) | .rodata | ≈32,000 | A |
| 17 | Battle message strings | .rodata | 21,696 obj | A |
| 18 | Map metadata (headers/events/connections/groups — `map_events.o` + `maps.o`) | .rodata | 117,928 + 29,580 | B/C — pointer-bearing metadata |
| 19 | Contest painting data | .rodata | 29,040 obj | A |
| 20 | Fonts (`fonts.o`) | .rodata | 8,192 | C — already ROM-hydrated via content.pak |

The `.rodata` section also carries 3,496,062 B of gc-discarded content
(unreferenced const data already dropped from the link — parity relics).

### 1.3 Classification of the ranked contributors (A/B/C/D per brief)

- **A (original payload → resource-owned):** ranks 1–12, 14–19 (scripts,
  text, tables, gfx leaves, multiboot blobs).
- **B (engine/runtime code + constants → stays compiled):** engine `.text`,
  `host_data` scratch, `gba_ewram`/`gba_common`/`game_bss` state, `.data`
  hydrated content-pak tables (until absorbed — see §5).
- **C (tiny routing/index tables that may stay):** `gMapLayouts` (1,764),
  `gMapGroups` + group tables (2,208), cry-ID routing (270), species→dex
  routing (822) — logical-ID-only tables; see architecture §pointer-shape D.
- **D (generated metadata, not game payload):** compat seam tables
  (`kPokemonBattleCompatResources` 38,592, `sPaletteRows`, `sLayoutRows`),
  debug/unwind metadata, linker-script plumbing.

## 2. Text inventory

**Total compiled text ≈ 874,022 B canonical** (≈4.2% of the release binary).
Encoding: GBA charmap single bytes (charmap.txt; `'A'`=0xBB, `'a'`=0xD5),
terminator `$`=0xFF, control codes `\l`=0xFA `\p`=0xFB `\d`=0xFC `\n`=0xFE,
placeholders `{PLAYER}`=FD 01 `{STR_VAR_1}`=FD 02, emoji `F9 xx`, battle
`{B_*}` placeholders. No NUL terminators, no length prefixes.

### 2.1 Script-side text (assembly sources → `script_data`) — 709,424 B exact

| Family | Canonical bytes | Notes |
|---|---|---|
| Map dialogue (468 `data/maps/*/scripts.inc`) | 291,269 | NPC talk, signs (226 sign labels), item scripts, gyms |
| BattleFrontier map dialogue | 136,835 | incl. BattleTowerMultiPartnerRoom 19,700 |
| `data/text/*.inc` (35 of 36 compiled) | 241,475 | trainers 67,095; match_call 53,617; apprentice 49,891; tv 25,160; braille; mart clerk |
| `data/scripts/*.inc` | 38,206 | mauville_man 8,811; contest_hall 7,643; secret_base 6,662 |
| Macro-generated `gText_` (Common_EventScript) | 1,639 | 74 labels |

Symbols: `_Text_` 6,743 + `gText_` 632 in `script_data`; main-game maps
10,298, BattleFrontier 3,289, scripts 1,326, battle AI 698, battle scripts
635, MatchCall 290, TV 186, other 2,058. Not compiled: 9 `gift_*.inc`
(mystery-gift text unlinked).

### 2.2 C-side text (→ `.rodata`) — ≈164,598 B canonical

| Family | Source | Canonical bytes | Arrays |
|---|---|---|---|
| Pokédex entries | `src/data/pokemon/pokedex_text.h` | 58,017 | 387 `g<Species>PokedexText` |
| Pokédex category names | `pokedex_entries.h` | 3,156 | struct-embedded |
| System/menu strings | `src/strings.c` | 31,258 | 1,772 arrays |
| Move descriptions | `move_descriptions.h` | 17,251 | 355 |
| Item descriptions | `item_descriptions.h` | 15,101 | 310 |
| Battle strings | `src/battle_message.c` | 12,001 | 524 |
| Easy chat words | 18 `easy_chat_group_*.h` | 7,101 | 1,008 |
| Abilities / ribbons / natures / trainer classes | several headers | 3,960 | — |

Excluded from compiled text: `species_names.h` / `move_names.h` are out
under `DESKTOP_EXTERNAL_GAME_CONTENT`; `gSpeciesNames` / `gMoveNames` live
in `host_data`, zero-filled at link and runtime-populated from the legacy
content package (see §5.1). Mail text is composed at runtime from easy-chat
words.

### 2.3 Pointer/table ownership, deduplication

- **Every text reference points to a string START; zero interior pointers.**
  Verified: `readelf` on `event_scripts.o` — 16,651 `R_X86_64_32` relocs,
  **0 nonzero addends**.
- Scripts reference text via `msgbox`/`trainerbattle`/`loadword` operands
  (`.byte SCR_OP_LOAD_WORD; .byte 0; .int \text` → native VA). The native
  `struct Trainer` has **no dialogue members** — trainer intro/lose text is
  reached only via script operands.
- Pointer tables (all → starts): `gStdStrings` 26×8 (`script_menu.o`),
  `gPokedexEntries[].description`, `gMoveDescriptionPointers` 354×8,
  `gNatureNamePointers`, `gItems[].description`, `gAbilityDescriptionPointers`
  80×8. Inline 2-D arrays (no pointers): `gAbilityNames` 78×13,
  `gTypeNames`, `gTrainerClassNames`.
- Deduplication: **1.79% duplicated** — 205 duplicate-content groups
  (27 with >2 copies) = 17,046 B across 952,183 B of measured text span.
  Strings are shared by symbol reference, not by content-canonicalization.

### 2.4 Native representation today

Text bytes in the binary are **byte-identical GBA charmap bytes** (verified
by in-binary decoding). Script operands are 4-byte native VAs
(`R_X86_64_32`, 0 addends; binary is non-PIE). At runtime every script
operand read flows through `HostResolveGbaAddr`
(`ScriptReadPointer` scrcmd.c:72–75; `T1_READ_PTR`/`T2_READ_PTR`), whose
resolution order is: exact-start logical table → interval ranges → identity
fallback. **Migration implication: text cutover = register text-label
logical addresses (the R12 audio pattern); zero consumer edits.**

## 3. Script bytecode inventory

`script_data` (1,065,814 B, writable) per-object:

| Object | Bytes | Relocs (4B / 8B) |
|---|---|---|
| `event_scripts.o` (field/event + map scripts + asm text) | 975,114 | 16,651 / 787 |
| `battle_anim_scripts.o` | 63,811 | 4,231 / — |
| `battle_scripts_1.o` | 13,099 | 1,508 / — |
| `battle_ai_scripts.o` | 9,303 | 1,222 / — |
| `contest_ai_scripts.o` | 2,524 | 364 / — |
| `field_effect_scripts.o` | 817 | 170 / — |
| `battle_scripts_2.o` | 493 | 54 / — |
| `mystery_event_script_cmd_table.o` | 136 | — |

### 3.1 Family inventory

| Family | Root symbols | Byte span | Interior pointers |
|---|---|---|---|
| Field/event scripts (`*_EventScript_*`) | 7,272 | ≈199,090 | yes — call/goto/msgbox/applymovement operands |
| Map scripts (`*_MapScripts`) | 470 | 3,289 | 4-B script pointers |
| Movement scripts (`*_Movement_*` + 8 `sMovement_*`) | 1,055 | 7,428 | **zero** (pure 1-byte opcodes) — **migrated in R13-B** (1,047 pret labels 7,404 B + 8 objects 24 B; the 3 recomp-local labels, 12 B, stay compiled) |
| Battle scripts (`BattleScript_*`) | 639 | 12,682 | yes |
| Battle anim scripts (unprefixed labels) | 656 | 64,206 | yes |
| Battle AI scripts (`AI_*`) | 555 | 9,304 | yes |
| Contest AI scripts (`AI_*`) | 165 | 2,524 | yes |
| Mystery event cmd table | 17×8 | 136 | fn pointers |
| Mystery gift scripts | 14 | 828 (.rodata) | yes (v-address scheme) |
| Field effect scripts (`gFieldEffectScript_*`) | 67 | 550 | fn + palette pointers |

### 3.2 Per-VM format

All VMs use **1-byte opcodes without a length byte**; operand layout is
per-command fixed (macro-defined). Command tables (8-byte native
`ScrCmdFunc*` entries): field 228, battle 249, battle anim 48, battle AI 99,
contest AI 136, field effect 8, mystery event 17. Address-bearing operands:
`call`/`goto` 1+4, `goto_if`/`call_if` 1+1+4, `applymovement` 1+2+4,
`loadword` 1+1+4, `accuracycheck` 1+4+2, `anim_call` 1+4, …

**The key native fact:** `.int label` emits `R_X86_64_32` and the host
linker patches the target's **native 32-bit address** (link is non-PIE, all
data < 4 GiB, truncation exact). These are NOT GBA logical addresses.
Byte-verified (`call` operand = 0x013e7a9b = native address of
`PetalburgCity_EventScript_MoveGymBoyToWestEntrance`). 8-byte tables
(`.quad` under LINUX64): `gScriptCmdTable`, `gSpecials` 526×8,
`gStdScripts` 11×8, `gSpecialVars` ~22×8, `gMysteryEventScriptCmdTable`,
map-event struct pointer fields 1,891×8.

### 3.3 Operand counts (relocation-based, exact)

| Object | 4B operands |
|---|---|
| event_scripts | 16,651 |
| battle_anim | 4,231 |
| map_events (table ptrs) | 2,163 (+1,891×8B) |
| battle_scripts_1 | 1,508 |
| battle_ai | 1,222 |
| contest_ai | 364 |
| field_effect | 170 |
| battle_scripts_2 | 54 |
| mystery_gift | 53 (+5×16-bit) |

Source-level macro counts: `goto` 1,279, `goto_if`/`call_if` 3,716,
`call` 877, `msgbox` 4,431, `applymovement` 2,009.

### 3.4 Running state and resolution

- `struct ScriptContext` (include/script.h): `u8 stackDepth, mode,
  comparisonResult; u8 (*nativePtr)(void); const u8 *scriptPtr;
  const u8 *stack[20]; ScrCmdFunc *cmdTable, *cmdTableEnd; u32 data[4]`.
  Field-VM instances `sGlobalScriptContext`/`sImmediateScriptContext` sit
  in **host .bss** (src/script.o) — outside serialized slices. Serialized
  slice holders: `sMysteryEventScriptContext` (EWRAM, full context),
  `gBattlescriptCurrInstr`, `gAIScriptPtr` (shared with contest AI),
  `sBattleAnimScriptPtr`/`sBattleAnimScriptRetAddr`, `gRamScriptRetAddr`,
  `sAddressOffset`, and the battle script stacks inside `gBattleResources`
  (heap → EWRAM slice).
- Every script-pointer operand read passes through `HostResolveGbaAddr`;
  its fallback is identity (`return (void*)(uintptr_t)addr` — host_memory.c)
  because operands are already native addresses. The exact-start logical
  registry is currently used only by the R12 audio seam.
- Mystery-gift v-commands: `ScrCmd_setvaddress` sets EWRAM `sAddressOffset`
  = operand − native opcode address; static scripts yield 0, RAM-copied
  scripts map by relative layout. The mystery-event VM is already
  native-refactored (`sMysteryEventScriptNativeBase` +
  `ResolveMysteryEventPointer`).

### 3.5 Cross-resource dependencies

script → text: 9,485 text symbols (7,375 in script_data, 2,110 in .rodata);
script → movement: 2,009 applymovement refs; script → script: 5,872
call/goto/goto_if refs; script → std/specials: `gStdScripts` 11, `gSpecials`
526 fn ptrs; map headers → scripts (map_events.o tables); battle script →
move-effect table 214 + `gBattleAnims_Moves` 365; cries/music are **IDs, not
pointers** (already isolated in R12).

### 3.6 Risk ranking

- **Highest risk: field/event scripts** — 975 KB single object, 16,651
  native pointer operands, 7,272 entry symbols, the `sAddressOffset`
  coupling, and interpreter state split across host .bss + serialized EWRAM.
- Second: battle anim scripts (4,231 operands).
- **Easiest: movement scripts** — 1,055 scripts / 7,428 B of pure 1-byte
  opcodes with **zero interior pointers** (only inbound applymovement refs);
  mystery-event cmd table (17×8) likewise trivial. **DONE as part of
  R13-B** (compiled → ROM_BASE pack, additive seam, no consumer redirect;
  see `docs/R13B_LEAF_PAYLOAD_MIGRATION_REPORT.md`).

## 4. Map metadata inventory

R11 removed the payload *leaves* (tileset gfx/metatiles/attributes,
blockdata, borders, object-event graphics, neighborhood inputs) — verified
absent as compiled data. **Every map metadata wrapper and index remains
compiled ≈ 1,129,250 B canonical:**

| Data | Count | Row B | Canonical B | Pointer fields | Location |
|---|---|---|---|---|---|
| MapHeader records | 518 | 44 | 22,792 | 4× host ptr (layout/events/scripts/connections) | .rodata 0xb3d91e |
| MapLayout records | 441 | 40 | 17,640 (28,200 fp) | border/map NULL (R11); 2× `gTileset_*` | .data 0xc031e0 (layout compat seam TU) |
| `gMapLayouts` index | 441 | 4 | 1,764 | GBA addr | .rodata 0xb3d23a |
| `gMapGroup_*` tables | 518 entries | 4 | 2,072 | GBA addr | .rodata 0xb43226 |
| `gMapGroups` | 34 | 4 | 136 | GBA addr | .rodata 0xb43a3e |
| MapConnections records+structs | 148+65 | 12/16 | 2,816 | struct → host ptr | .rodata 0xb43ac6 |
| ObjectEventTemplate | 428 tables, 2,776 entries | 24 | 66,624 | 4-B GBA script addr | .rodata 0xb20592 |
| WarpEvent | 444 tables, 1,313 | 8 | 10,504 | none | .rodata |
| CoordEvent | 62 tables, 375 | 24 | 9,000 | 8-B host script ptr | .rodata |
| BgEvent | 137 tables, 720 | 16 | 11,520 | 8-B host ptr / u32 id | .rodata |
| MapEvents structs | 507 | 40 | 20,280 | 4× host ptr | .rodata 0xb207da |
| Map scripts | 468 maps | — | 948,273 | 4-B script ptrs | script_data |
| RegionMapEntries | 213 | 16 | 3,408 | host name ptr | .rodata 0xac1160 |
| `sMapName_*` names | 196 | var | 2,413 | — | .rodata |
| WildPokemonHeader | 125 | 40 | 5,000 | 4× host ptr | .rodata 0xafcbc0 |
| Tileset structs | 74 | 48 | 3,552 | pack-published ptrs | .data |
| Tileset anims | 35 | var | 1,456 | — | .rodata |

Notes:

- `gMapHeader` (EWRAM, 0x30) is the runtime working copy populated by
  `LoadCurrentMapData`; the 518 compiled records are per-map const.
- 11 eventless maps (contest halls) have `events=NULL`.
- `gMapGroups`/`gMapGroup_*`/`gMapLayouts` hold 4-byte GBA addresses and
  are resolved via `HostResolveGbaAddr` at load (overworld.c) — the
  GBA-form routing pattern.
- `gWildMonHeaders` (125×40) holds 4 host pointers into the compiled wild
  encounter tables (see §5).

## 5. Gameplay/data tables inventory

### 5.1 The legacy content-package hydrate set (NOT compiled payload)

Five data tables + ten fonts are runtime-hydrated into writable `host_data`
from the legacy `content.pak` (EMRLDATA, `src/platform/desktop_game_content.c`,
built from the user's verified ROM on import):

| Table | Rows × size | Bytes | Notes |
|---|---|---|---|
| `gSpeciesInfo` | 412 × 26 | 10,712 | flattened numeric (base stats inline; no pointers) |
| `gBattleMoves` | 355 × 9 | 3,195 | effect/power/type/accuracy/pp/secondary/target/priority/flags |
| `gMoveNames` | 355 × 13 | 4,615 | text |
| `gSpeciesNames` | 412 × 11 | 4,548 | text |
| `gExperienceTables` | 8 × 101 × u32 | 3,232 | growth-rate curves |

`gBaseStats`, `gMovesInfo`, `gAbilitiesInfo`, `gFormSpeciesIdTable` do not
exist in this port at all. **This second-identity package is architecture
debt**: the migration doc (EMERALD_ROM_BACKED_ASSET_MIGRATION_ARCHITECTURE.md
§1/§21, "R9 — old-package convergence") requires it to be absorbed into the
Gen3 registry, not extended. Absorbing it is an R13 task even though the
bytes are not compiled payload.

### 5.2 Compiled species tables (`.rodata`)

| Table | Rows × size | Bytes | Pointer-bearing |
|---|---|---|---|
| `gEvolutionTable` | 412 × 5 × 6 | 12,360 | no (flat numeric; EVOS_PER_MON = 5) |
| `gLevelUpLearnsets` | 412 × 8 ptr table | 3,296 | yes → 411 leaf u16 arrays |
| s\*LevelUpLearnset leaves | 411 arrays | 8,766 | packed `(lvl<<9)\|move`, 0xFFFF term |
| `gTMHMLearnsets` | 412 × 8 bitfield | 3,296 | no |
| sTutorLearnsets | 412 × u32 | 1,648 | no |
| `gTutorMoves` | 30 × u16 | 60 | no |
| `gEggMoves` | flat u16 stream, 165 blocks | 2,278 | no |
| `gSpeciesIdToCryId` | 135 × u16 | 270 | no (R12 cry routing) |
| sSpeciesToNationalPokedexNum | 411 × u16 | 822 | no |
| `gPokedexOrder_Alphabetical/_Height/_Weight` | 411/386 × u16 | 2,366 | no |
| `gAbilityNames` / `gAbilityDescriptionPointers` | 78×13 / 80×8 | 1,014 / 640 | desc ptrs → text |

Consumers index `gSpeciesInfo`/`gEvolutionTable`/`gLevelUpLearnsets`
directly (pokemon.c, daycare.c, battle_util.c — no accessor layer).
`emerald_pokemon_native_compat.c` wraps only the R9 battle-**graphics**
tables, not species data.

### 5.3 Compiled move/contest tables (`.rodata`)

| Table | Rows × size | Bytes | Pointer-bearing |
|---|---|---|---|
| `gMoveDescriptionPointers` | 354 × 8 | 2,832 | text |
| `gContestMoves` | 355 × 7 | 2,485 | no (combo ids embedded) |
| `gContestEffects` | 48 × 3 | 144 | no |
| `gContestEffectFuncs` | 48 × 8 | 384 | **fn pointers** |
| `gComboStarterLookupTable` | 64 × 1 | 64 | no |
| `gContestEffectDescriptionPointers` | 48 × 8 | 384 | text |
| `gContestMoveTypeTextPointers` | 5 × 8 | 40 | text |

`gBattleMoves` is consumed by direct global access throughout
battle_util.c/battle_main.c. Move-effect routing: `HostResolveGbaAddr(
gBattleScriptsForMoveEffects[gBattleMoves[].effect])` — one hybrid seam
already exists (battle_util.c:285).

### 5.4 Compiled item tables (`.rodata`)

| Table | Rows × size | Bytes | Pointer-bearing |
|---|---|---|---|
| `gItems` | 377 × 72 | 27,144 | yes — desc text + 2 use-fn pointers |
| `gItemEffectTable` | 206 × 8 | 1,648 | yes → `gItemEffect_*` leaves |
| `gItemIconTable` | 378 × 2 × 8 | 6,048 | gfx (icons — gfx-leaf family) |

Native `struct Item` = 72 B (name embedded `u8[14]`, price u16, holdEffect
**enum**, description ptr, importance/registrability/pocket/type,
`fieldUseFunc`/`battleUseFunc` fn pointers). Consumers go through thin
accessors (`GetItemName`, `GetItemPrice`, `GetItemHoldEffect`, …,
item.c:875–902) — a usable seam. **Shops have no compiled inventory** —
mart item lists are runtime u16 lists fed by the script engine
(`sMartInfo.itemList`, EWRAM).

### 5.5 Trainers, encounters, frontier, misc (`.rodata` unless noted)

| Family | Table | Count × row | Bytes | Pointer-bearing |
|---|---|---|---|---|
| Trainers | `gTrainers` | 855 × 48 | 41,040 | 1 party ptr/row |
| Trainers | `sParty_*` | 854 tables, 1,825 mons | 15,066 | no (u16 rows) |
| Trainers | `gTrainerClassNames` | 66 × 13 | 858 | no |
| Trainers | trainer pic pixel data | — | absent | R9-migrated |
| Frontier | `gBattleFrontierTrainers` | 300 × 56 | 16,800 | 1 ptr/row |
| Frontier | `gBattleFrontierTrainerMons_*` | 300 tables | 28,060 | no |
| Frontier | brains + facility aux (dome/factory/palace/arena/pike/pyramid/prizes) | ≈35 tables | ≈11,900 | some text ptrs, code ptrs |
| Bard | `sBardSoundTemplates_*` | 20 category tables | 64,076 | no |
| Weather | `sDroughtWeatherColors` | 16×16×96×2 | 49,152 | no |
| Easy chat | `gEasyChatGroups` + 22 word tables + names | 22×16 + 8,868 words | 18,294 | 1 ptr/group |
| Encounters | `gWildMonHeaders` | 125 × 40 | 5,000 | 4 ptrs/row |
| Encounters | WildPokemonInfo | 209 × 16 | 3,344 | 1 ptr/row |
| Encounters | WildPokemon rows | 2,070 × 4 | 8,280 | no |
| Encounters | `sRoamerLocations` | 21 × 6 | 126 | no |
| Pokédex | `gPokedexEntries` | 387 × 40 | 15,480 | 2 text ptrs/row |
| Contests | `gContestOpponents` | 96 × 64 | 6,144 | no |
| Match call | `sMatchCallTrainers` + text/fn tables | 64×18 + 10+7 tables | ≈6,530 | text + fn ptrs |
| Decorations | `gDecorations` | 121 × 40 | 4,840 | 2 text ptrs/row |
| Braille | `sFont_Braille` | 256 × 16 | 4,096 (+118) | no |
| TV | 29 `sTV*TextGroup` tables | — | 2,656 | text ptrs |
| Pokénav | `sPokenavCityMaps` + 22 city-map blobs | 22 | 1,980 | gfx ptrs |
| Berries | `gBerries` + crush data | 43 × 40 / 43 × 4 | 1,892 | 2 text ptrs/row |
| Landmarks | `sLandmarkLists` + rows | 47 × 16 + rows | ≈1,450 | text ptrs |
| Secret base | 9 small tables + 8 decoration blobs | — | ≈580 | some |
| Small | heal locations 132; lottery 8; prize items 16; lilycove 6 | — | ≈162 | no |
| Type chart | `gTypeEffectiveness` | 56 × 6 | 336 | no |
| Multiboot | Colosseum program | 1 | 163,840 | leaf blob |
| Multiboot | ereader program | 1 | 25,024 | leaf blob |
| Multiboot | berry glitch fix | 1 | small | leaf blob |

Revision-delta flags (release binary vs current source, to resolve before any
extraction manifest is cut): wild maps 125 vs 124 in
`src/data/wild_encounters.json`; WildPokemonInfo 209 vs 220; wild rows 2,070
vs 2,107; PikeWildMon 16-B entries vs current layout; `gEasyChatGroups`
16-B rows vs current source; `gDecorationDesc`/`gDecorationGfx` symbols
absent in the binary. Absent by design (not compiled): `gStaticMonsterTable`,
`gSwarms`, `gShopInventories`/`sMartInventories` (mart lists are
script-driven), dewford-trend phrases (script text), `gContestEffectCombos`.

## 6. State-v5 pointer-surface audit (summary)

The v5 sidecar machinery needs **no changes** for R13: every serialized
pointer field listed below is already caught by the range-lookup walker; the
work is registering per-family ranges and routing `HostResolveGbaAddr`
through the logical registry. Per-save sidecar records:

- Field save, typical: `gMapHeader` 4 (mapLayout/events/mapScripts/
  connections) + `gFonts` 1 + ScriptContext 1–3 + active TextPrinters 0–4 +
  placeholder strings 0–2 ≈ **6–14 records**.
- Mid-battle save: +1–5 script pointers + 1 AI ptr + 1–6 battle script
  stack entries + 0–9 trainer speech ptrs ≈ **10–35 total**.
- Frontier challenge: +2 (`gFacilityTrainers`, `gFacilityTrainerMons`).
- Worst plausible case ≈ 60 — two orders of magnitude below the 4,096 cap
  (~1,005 used by audio today; range-index cap 8,192 with ~2,040
  trainer/pokemon + audio ranges registered).
- Species/move/item/trainer/encounter tables have **essentially zero
  serialized pointer surface** — all consumers are transient; nothing
  caches a row pointer (daycare copies u16 move IDs, not pointers).

Serialized pointer fields by family: ScriptContext (`scriptPtr` + `stack[20]`
+ `cmdTable*`), battle script registers (`gBattlescriptCurrInstr`,
`gAIScriptPtr`, `sBattleAnimScriptPtr/RetAddr`, battleScriptsStack),
trainer speech buffers (9 × `u8*` in EWRAM, cleared at battle end),
`gApproachingTrainers[2].trainerScriptPtr`, active TextPrinters'
`currentChar` (0–4 live; already specially modeled in native_state.c),
`gFonts` (COMMON_DATA), `sStringPointers[8]`, `gMapHeader` copy (4 ptrs),
frontier facility tables. The neighborhood compat object holds map pointers
but lives in host .bss (not serialized) and rebuilds from numeric identity —
the proven reconstruction pattern.

## 7. Consumer seams (one line per family)

- **Text:** all reads flow through `HostResolveGbaAddr` (ScriptReadPointer
  + T1/T2_READ_PTR); migration = logical-address registration, **zero
  consumer edits**.
- **Scripts:** same path; register script-table labels via
  `HostMemoryRegisterLogicalAddress` (the R12 audio pattern).
- **Map headers/events:** seam exists (`Overworld_GetMapHeaderByGroupAndId`
  + `GetMapLayout`, PORTABLE branch); direct `gMapHeader.` consumers remain
  (fieldmap.c).
- **Species/moves:** extend the desktop content-package hydration with
  Republish-style arena repointing (R9 pattern); tables are outside all
  serialized slices.
- **Items:** no accessor layer for struct fields beyond thin helpers —
  migrate by relocating/aliasing `gItems` (Republish pattern), not
  accessor surgery.
- **Trainers/encounters:** same Republish approach; only the speech buffers
  and frontier tables touch state.
