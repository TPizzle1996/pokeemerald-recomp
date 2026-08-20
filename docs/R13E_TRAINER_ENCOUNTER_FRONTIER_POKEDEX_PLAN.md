# R13-E — Trainer / Encounter / Frontier / Pokédex Migration Plan

Stage scope: migrate trainers + parties, wild encounters, Battle Frontier /
facilities, and Pokédex structured data (and the deferred R13-C text that
binds to them) into ROM-owned canonical resources. **Design/implementation
review only — no production code is modified by this document. No commit.
R13-F not started.**

Grounding: `docs/R13_REMAINING_CONTENT_AUDIT.md` (R13-A),
`docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md`, `docs/R13D1/…`, `docs/R13D2/…`,
`docs/R13C_TEXT_MIGRATION_REPORT.md`, `docs/R10_NATIVE_STATE_V5_DESIGN.md`,
and the current R13-D2-complete tree re-audited against the qualified Emerald
reference ELF (`../pokeemerald-reference/pokeemerald.elf`) + retail ROM
(SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).

Current proven state entering R13-E:

| Quantity | Value |
|---|---|
| Production pack | **15,750 entries**, 13,190,320 B, SHA-256 `71a0ba0e…95291` |
| Pack cap | 16,384 entries (merged-import cap 16,384) |
| Registered resource ranges | 5,847 / 8,192 |
| Text ownership | 3,429 ROM_BASE_ONLY / 1,758 COMPILED_PENDING_MIGRATION |
| Seams landed | R9 gfx, R12 audio, R13-B leaf, R13-C text, R13-D1 gameplay + fonts, R13-D2 items |

---

## 1. Exact R13-E inventory (pending agent refinement; R13-A canonical sizes)

Sizes below are the R13-A audit canonical figures; the inventory agents
re-derive exact ROM-derived row counts / bytes / consumers and reconcile
against the current R13-D2-complete tree.

### Trainers
| Family | Count × row | Canonical bytes | Native compiled | Pointer-bearing | Notes |
|---|---|---|---|---|---|
| `gTrainers` | 855 × 40 | 34,200 | 41,040 (×48) | 1 party ptr/row | transformed (party ptr → arena); GBA 40 B / native 48 B |
| trainer parties `sParty_*` | 854 tables | 18,088 | 18,088 | none (u16 rows) | party leaves (4 struct variants); ~0x30b62c..0x30f8cc block |
| `gTrainerClassNames` | 66 × 13 | 858 | 858 | fixed-width rows | structured name rows |
| trainer pic pixel data | — | absent | — | — | R9-migrated (trainer_front/back) |
| AI flags / double-battle | in-row fields | (in gTrainers) | — | — | no separate table |
| trainer dialogue | — | — | — | — | NOT in gTrainers rows; lives in map scripts (R13-F/G); text strings R13-C-owned |

### Wild encounters
| Family | Count × row | Canonical bytes | Pointer-bearing | Notes |
|---|---|---|---|---|
| `gWildMonHeaders` | 125 × 40 | 5,000 | 4 ptrs/row | header→info pointer graph; **125-vs-124 delta** |
| WildPokemonInfo | 209 × 16 | 3,344 | 1 ptr/row | per-type info (land/water/fishing); **209-vs-220 delta** |
| WildPokemon rows | 2,070 × 4 | 8,280 | none | species/level slots; **2,070-vs-2,107 delta** |
| `sRoamerLocations` | 21 × 6 | 126 | none | roaming legendaries |
| static/special encounters | — | — | — | event-flag driven (scripts), not a table |

### Battle Frontier / facilities
| Family | Count × row | Canonical bytes | Pointer-bearing | Notes |
|---|---|---|---|---|
| `gBattleFrontierTrainers` | 300 × 56 | 16,800 | 1 ptr/row | shared frontier trainer pool |
| `gBattleFrontierTrainerMons_*` | 300 tables | 28,060 | none | frontier mon sets (custom moves) |
| brains + facility aux (dome/factory/palace/arena/pike/pyramid/prizes) | ≈35 tables | ≈11,900 | some text ptrs, code ptrs | facility-scoped; includes PikeWildMon |
| rental pools (factory) | in facility aux | — | — | level-50/open rental sets |
| apprentice | 49,891 text + structured | — | — | text R13-C; structured part TBD |

### Pokédex
| Family | Count × row | Canonical bytes | Pointer-bearing | Notes |
|---|---|---|---|---|
| `gPokedexEntries` | 387 × 32 | 12,384 | 15,480 (×40) | 1 text ptr/row (description) | categoryName inline (12 B); desc→R13-C text re-point |
| `sSpeciesToNationalPokedexNum` | 411 × u16 | 822 | none | species↔dex routing |
| `gPokedexOrder_Alphabetical/_Height/_Weight` | 411/386 × u16 | 2,366 | none | dex ordering |
| dex area/region maps | gfx | — | gfx ptrs | gfx = later leaf stage, not E |
| R13-C pokedex text | 387 labels | ~58,017 | — | currently COMPILED_PENDING_MIGRATION → flip ROM_BASE_ONLY |

### Other (R13-A listed under E; §10 decides IN vs DEFER)
| Family | Count × row | Canonical bytes | Recommendation |
|---|---|---|---|
| easy chat words (`gEasyChatGroups` + 22 tables) | 22×16 + 8,868 words | 18,294 | DEFER (bundled text + 16-B row delta) |
| bard templates | 20 tables | 64,076 | DEFER (R13-B leaf wave) |
| weather palette table | 16×16×96×2 | 49,152 | DEFER (R13-B leaf wave) |
| contests `gContestOpponents` | 96 × 64 | 6,144 | DEFER (contest structured) |
| match call | 64×18 + 10+7 tables | ≈6,530 | DEFER (text + fn ptrs) |
| decorations `gDecorations` | 121 × 40 | 4,840 | DEFER (2 text ptrs/row) |
| berries `gBerries` | 43 × 40 / 43 × 4 | 1,892 | DEFER (2 text ptrs/row) |
| TV | 29 tables | 2,656 | DEFER (text ptrs) |
| landmarks | 47×16 + rows | ≈1,450 | DEFER (text ptrs) |
| secret base | 9 tables | ≈580 | DEFER |
| small (heal locations/lottery/prize) | — | ≈162 | DEFER |
| type chart | 56 × 6 | 336 | DEFER (engine mechanics constant) |
| braille font | 256 × 16 | 4,096 | DEFER (R13-B leaf) |

Rationale for deferral (§10): R13-E stays focused on the four core families
(trainers/encounters/frontier/pokedex) + the deferred text that binds to them.
The misc structured/leaf families above are either (a) R13-B leaf-wave
material (bard, weather, braille), (b) text-heavy with fn/pointer graphs that
belong with the R13-C/G text machinery (matchcall, TV, easy-chat,
decorations, berries, landmarks), or (c) engine constants (type chart). None
is trainer/encounter/frontier/pokedex content, so absorbing them now would
turn E into a dumping ground without adding to E's cutover boundary.

---

## 2. Revision-delta gate (re-audit; classification A/B/C/D)

R13-A flags (release binary vs current source, resolve before cutting any
manifest). **All four encounter-family flags are now RESOLVED as
non-divergences** (inventory agents, BPEE01). Classifications:

| # | Flag | Classification | Resolution |
|---|---|---|---|
| 1 | wild maps **125 vs 124** | **(A) bookkeeping + (B) sentinel** | ROM/source/native all carry 125 rows = 124 real maps + 1 `MAP_UNDEFINED` NULL terminator; the JSON omits the sentinel. No content delta. |
| 2 | WildPokemonInfo **209 vs 220** | **(B) counting artifact** | 209 = reachable via gWildMonHeaders; 220 = total incl. Pike (4) + Pyramid (7). ROM/source/native = 220. No delta. |
| 3 | wild rows **2,070 vs 2,107** | **(B) stale audit arithmetic** | actual = 2,107 everywhere (106×12+55×5+6×5+53×10 = 2,107 = 8,428 B). No delta. |
| 4 | **PikeWildMon** 16-B entries | **(B) representation bookkeeping** | `struct PikeWildMon` = 12 B on both GBA and native; table contents byte-identical. The "16-B" flag refers to the native WildPokemonInfo row, not PikeWildMon. No delta. |
| 5 | `gEasyChatGroups` 16-B rows | **(B) pointer-width layout** | 22 × 8-B GBA / 16-B native; vanilla-faithful. Easy-chat is deferred (out of E1). |
| 6 | `gDecorationDesc`/`gDecorationGfx` absent | DEFER (decorations out of E) | n/a for E1 |
| 7 | `gStaticMonsterTable`/`gSwarms`/`gShopInventories`/`gContestEffectCombos` absent | **(B) absent by design** | static legendaries are script-driven (`setwildbattle`); swarms save-block; mart lists script-driven. Not compiled tables. |

Doc corrections carried: `gPokedexEntries` has **1 text pointer/row (description)**;
`categoryName` is an inline 12-byte Gen-3 string (not a pointer) — R13-A §5.5
"2 text ptrs/row / 40 B" conflated native row size and pointer count
(`32 B` GBA / `40 B` native).

For each still-compiled trainer/frontier/pokedex family, the D1/D2 parity-gate
pattern applies; any unexplained divergence = STOP. The D1 (evolution) and D2
(6 held-item overrides) precedent applies: deliberate fork behaviour is
preserved via an explicit machine-checked override, never silently replaced.
For E1 the trainer parity gate is the authoritative trainer verdict (see the
E1 implementation).

---

## 3. Resource granularity — trainers

Tallgrass target:
```
override trainer/youngster-calvin
override trainer/youngster-calvin/party
```
without replacing unrelated trainers.

Split (derived from the ROM layout):
- `emerald:data/trainer/<trainer>` — the 48 B gTrainers row with the party
  pointer field REMOVED/nulled in the canonical payload (the pointer is a
  derived field reconstructed by the seam from the party resource). Canonical
  = exact ROM slice minus the interior pointer; the party linkage is expressed
  by the key (`…/<trainer>/party`), not stored as an address.
- `emerald:data/trainer/<trainer>/party` — the party leaf (one of the 4
  struct variants; see §4). NOT one giant resource.

Trainer metadata fields in-row: name[12] (fixed-width charmap), trainerClass
u8, encounterMusic_gender u8, trainerPic u8, doubleBattle u8, partyFlags u8
(bits: F_CUSTOM_MOVES, F_CUSTOM_ITEMS? determine exact), items[4] u16, aiFlags
u32, party ptr. Name is structured fixed-width (like species/move names), so
it rides inside the trainer resource — no separate text identity.

Party-struct variants: pret Emerald parties use 4 layouts depending on
partyFlags bits:
- `TrainerMonNoItemNoMoves` {iv u8, lvl u8, species u16} (8 B? verify)
- `TrainerMonItemNoMoves` {iv, lvl, species, item u16}
- `TrainerMonNoItemMoves` {iv, lvl, species, moves[4] u16}
- `TrainerMonItemMoves` {iv, lvl, species, item, moves[4]}
The canonical party resource must be **tagged** with which variant (the
partyFlags bits are in the trainer metadata row), OR use separate schemas per
variant. Decision: one schema family `trainer-party` with a `variant` tag
derived from the trainer row's partyFlags (canonical bytes stay exact ROM
slice; the tag is metadata, not payload). This keeps the payload byte-exact
while letting the seam choose the correct native struct layout.

Count: 855 trainer rows + 854 party leaves (+ alias dedup — determine how many
parties are shared) → ~1,700 trainer resources. `gTrainerClassNames` 66 rows
as `emerald:data/trainer-class/<class>` (66) or one whole-table resource —
Tallgrass mod-friendliness favors per-class, but 66 fixed-width rows are tiny;
recommend per-class keys.

## 4. Trainer pointer graph + publication (analogous to R13-D2 gItems)

`gTrainers` is TRANSFORMED_STRUCTURAL + GBA_POINTER_GRAPH (analogous to the
D2 gItems transform + the D1 levelup arena rebuild):

- canonical trainer row (48 B, party ptr nulled) + canonical party leaf →
  transformed native `gTrainers` row (native `struct Trainer` — determine
  native size; the native row holds a HOST party pointer) → native party
  pointer into a published party arena.
- No native pointers in pack payload (the pack holds GBA wire; the seam
  resolves the party pointer to the arena).
- Publication: a D2-style transform fills `HOST_DATA struct Trainer
  gTrainers[855]`; party leaves are concatenated into an arena and the
  trainer row's party pointer is set to the arena offset for its party. The
  party variant (which struct) is selected by partyFlags.
- Consumer edits: `src/battle_*` and party-building code index
  `gTrainers[i]` directly; keeping the symbol + native layout means zero mass
  edits (the D1/D2 precedent).

## 5. Trainer text interaction

`struct Trainer` has **no dialogue members** (R13-A §3 note) — trainer
intro/lose/post-battle text lives in **map scripts** (`trainerbattle`
operands), which are R13-F/G scope. Trainer NAMES are fixed-width structured
(in-row), not R13-C text resources. Trainer CLASSES (`gTrainerClassNames`) are
fixed-width rows (§3). Frontier trainer text is likewise script-side.
Therefore R13-E does NOT flip any trainer dialogue text; trainer-related
R13-C flips are limited to any deferred label family that a migrated table's
pointer targets (none for gTrainers/parties; see Pokédex for its flips).

## 6. Wild-encounter resource model

Map/profile-scoped identities, mirroring the ROM's header→info→slot graph:
```
emerald:data/encounter/<map>/land
emerald:data/encounter/<map>/water
emerald:data/encounter/<map>/fishing
(emerald:data/encounter/<map>/rock-smash  if applicable)
```
`<map>` = the map's semantic name (the R13-F map namespace must align; use a
stable group/num-derived key now so E and F don't collide). Design to avoid
coupling E to F:
- `gWildMonHeaders` is published as a transformed header table (row per map,
  the 4 info pointers nulled in the canonical payload).
- Each distinct info table (land/water/fishing slot arrays) is a separate
  resource; the header row references them by the encounter key, not an
  address. Shared/aliased tables are one resource referenced by multiple
  headers.
- Maps with no encounters: no resource; the header row marks the pointer null.
- Canonical slot rows: land 12 slots × {minLevel,maxLevel,species}, water 5
  slots, fishing 10 slots (verify exact slot counts from ROM).
- Slot-row resources (`WildPokemon rows` 2,070) are the leaf payloads; the
  info structs (209) are the per-type pointer rows that reference them.

## 7. Encounter live-consumer seam (minimum cutover)

Trace: map → `gWildMonHeaders` (matched by mapGroup/mapNum) → info struct →
slot table → species/level selection (`src/wild_encounter.c`). Determine
whether the native code matches by mapGroup/mapNum (ID) or caches a header
pointer. Minimum cutover: a transformed `gWildMonHeaders` table (HOST_DATA,
same symbol) + a generated/arena info-pointer rebuild; **do not rewrite
encounter selection logic**. If the native code resolves by mapGroup/mapNum
(ID), the cutover is a pure table republish (like D1). If it caches pointers,
add a logical-address mapping. The agents determine which.

## 8. Battle Frontier data classification

Per-family classification (agent-verified; R13-A sizes):

| Family | Class | Granularity |
|---|---|---|
| `gBattleFrontierTrainers` (300×56) | TRANSFORMED_STRUCTURAL + POINTER_GRAPH | per frontier-trainer |
| `gBattleFrontierTrainerMons_*` | LEAF_BYTES (u16 mon/move rows) | per frontier-trainer mons set |
| dome/factory/palace/arena pools | LEAF_BYTES / POINTER_GRAPH | per facility pool |
| PikeWildMon | LEAF_BYTES (16-B entries — delta) | whole-table |
| Pyramid item pools / prize tables | LEAF_BYTES | per facility pool |
| rental Pokémon (factory) | LEAF_BYTES | per rental set |
| brains (frontier bosses) | TRANSFORMED_STRUCTURAL (text ptrs) | per facility |
| streak/level/RNG formulas | ENGINE_CONSTANT | stay compiled |
| facility callback/fn tables | ENGINE_CONSTANT | stay compiled |

Separate original-content (trainer/move/item pools) from engine (algorithms,
RNG, callbacks). Mod-friendly: per-trainer + per-pool-set identities; avoid
one giant facility blob, but don't explode counts (facility pools are
whole-table resources where they are internally coherent).

## 9. Pokédex strategy

`gPokedexEntries` 387 × 40: fields = categoryName (text ptr), height u16,
weight u16, description (text ptr), pokemonScale/Offset, trainerScale/Offset.
The category + description pointers target R13-C **pokedex text resources**
(387 labels, currently COMPILED_PENDING_MIGRATION). Strategy:
- `emerald:data/pokedex/<species>` — the 40 B row with the 2 text-pointer
  fields re-pointed (D2-style: validate the pointer binds to the matching
  `emerald:text/pokedex/<label>` resource, fill the arena pointer).
- `override pokedex/treecko` / `/category` / `/description` — category +
  description are the existing R13-C text identities (re-point, don't
  duplicate); height/weight/scale ride in the row resource.
- Ordering tables (`gPokedexOrder_*`, `sSpeciesToNationalPokedexNum`) →
  whole-table LEAF_BYTES resources (they are ID routing, not text).
- R13-C flips: the 387 pokedex text resources → ROM_BASE_ONLY.
- Dex area/region maps are gfx → later leaf stage, not E.

## 10. Easy-chat / berry / misc boundary

**IN E:** trainers + parties, wild encounters, frontier, pokedex, + the
deferred R13-C text that binds to those tables (pokedex 387 labels).
**DEFERRED:** easy-chat (bundled text + 16-B row delta), berries, cable-club,
apprentice, ribbon/frontier-helper tables, contests, match-call, decorations,
landmarks, TV, secret-base, small tables, type chart, bard/weather/braille
(R13-B leaf wave). Rationale: keep E's cutover boundary on the four core
families; the misc families have different cutover dependencies (text-graph,
fn-pointers, leaf machinery) and would otherwise bloat E without improving
its isolation boundary.

## 11. Pack-cap arithmetic

Current 15,750 / 16,384 (headroom 634). R13-E adds roughly:
- trainers ~855 + parties ~854 + classes 66 ≈ 1,775
- encounters ~125 headers + ~209 info + ~2,070 slot rows (shared, dedup to
  distinct) — distinct resources likely ~300–500 depending on aliasing
- frontier ~300 trainers + ~300 mon sets + ~35 facility ≈ 635
- pokedex ~387 + ~4 ordering ≈ 391

R13-A projected "≈ 3,500 rows/tables". Taking ~3,000–3,500 new resources →
pack ≈ 15,750 + ~3,300 ≈ **19,000–19,250 entries**. This exceeds 16,384, so a
cap raise is required.

Proposed cap: **32,768**. Arithmetic:
- E ≈ 19,000–19,250 (fits; ~13,500 headroom at 32,768).
- R13-F ≈ +700 map bundles → ~19,950.
- R13-G ≈ +468 script bundles → ~20,400.
- R13-H ≈ +1,800 battle/anim/AI roots → ~22,200.
- Remaining R13-B leaf wave ≈ +2,800 → ~25,000.
All fit under 32,768 with headroom. 24,576 covers E alone but would need
another raise by R13-G/H (~22,200 fits, but the remaining leaf wave ~25,000
does not). Since each cap raise is a single constant and the whole remaining
program clearly exceeds 24,576, **raise once to 32,768** (pack-entry cap +
merged-import cap + the 16,385-refusal test pin). Do NOT confuse with the
State-v5 range cap (8,192) — unchanged.

## 12. State-v5

R13-A §6: species/move/item/trainer/encounter tables have **zero serialized
pointer surface** (all consumers transient). Frontier: +2 serialized table
pointers (`gFacilityTrainers`, `gFacilityTrainerMons`) — these are arena
ranges, handled by registering COMPAT_OBJECT ranges. Pokédex: category/
description arena pointers → already-covered R13-C text-arena routing
(currentChar relocation). No per-resource ranges; only arena/family ranges.
Current range count 5,847 / 8,192; E adds at most a handful of family ranges
(trainer arena, party arena, encounter arena, frontier arenas, pokedex arena)
→ post-E ≈ 5,860, well under cap. No State-v5 format change; no range-cap
raise.

## 13. Resource namespaces / Tallgrass scoping

All keys stay **profile-scoped** `emerald:data/…` (never global `gen3:data`).
Future Tallgrass layers canonical species/mechanic abstractions ABOVE these
game-profile resources; a ROM-hack importer produces overrides by emitting
MOD-provider resources with the same `emerald:data/…` keys (resolver
precedence picks the MOD over ROM_BASE). Examples per family in §17.

## 14. Native publication architecture

Recommend a **hybrid** (one seam per logical family, reusing the D1
`EmeraldGameplayCompat` structure rather than a single mega-seam):
- **EmeraldTrainerCompat** — gTrainers + parties + class names.
- **EmeraldEncounterCompat** — gWildMonHeaders + info + slots.
- **EmeraldFrontierCompat** — frontier trainers/mons/facility pools.
- **EmeraldPokedexCompat** — gPokedexEntries + ordering tables + text flips.
Rationale: families have independent pointer graphs and independent STOP
conditions; separate seams keep each cutover/rollback atomic. Avoid tiny
modules for the small whole-table families (ordering tables, class names) —
fold them into the nearest family seam. Publication order: trainers →
encounters → frontier → pokedex (no hard cross-family dependency; the only
cross-stage dep is R13-F using encounter headers, which E publishes before F).

## 15. Live-cutover strategy

| Family | Resources | Canonical B | LIVE after E? | Compiled removed in E? | Reason |
|---|---|---|---|---|---|
| gTrainers | ~855 | 41,040 | yes | yes | parity-pass; D2-style transform |
| trainer parties | ~854 | 15,066 | yes | yes | leaf arena rebuild |
| gTrainerClassNames | 66 | 858 | yes | yes | fixed-width rows |
| gWildMonHeaders | ~125 | 5,000 | yes (if delta resolves) | yes | delta gate first |
| encounter info/slots | ~300–2,070 | ~11,624 | yes (if delta resolves) | yes | delta gate first |
| gBattleFrontierTrainers | ~300 | 16,800 | yes | yes | pointer graph |
| frontier mon sets | ~300 | 28,060 | yes | yes | leaves |
| frontier facility aux | ~35 | ~11,900 | partial | partial | text/fn-ptr rows stay ENGINE_CONSTANT |
| gPokedexEntries | ~387 | 15,480 | yes | yes | text re-point |
| dex ordering | ~4 | 2,366+822 | yes | yes | whole-table |
| pokedex text | 387 | ~58,017 | flip→ROM_BASE | yes | R13-C deferred flip |

Avoid a giant COMPILED_PENDING_MIGRATION tail: every E family that passes its
parity/delta gate cuts over LIVE in E; only the ENGINE_CONSTANT frontier
aux/brain text-ptr rows and the deferred misc families (§10) stay compiled.

## 16. Isolation

Per-family proofs (no broad "frontier data"/"trainer data" exemption):
- trainer rows: symbol absence of compiled gTrainers + whole-table hash.
- trainer party leaves: leaf-symbol sweep (all sParty_* absent) + arena
  byte equality.
- encounter headers/slots: symbol absence of gWildMonHeaders + whole-table
  hash + per-row scans.
- frontier: symbol absence per table + whole-table hashes; short-row
  (≤8 B) precise exemptions.
- pokedex rows: symbol absence of compiled gPokedexEntries + 387-row scan +
  the 387 flipped text labels absent (R13-C grammar).
- newly-flipped text: R13-C isolation pattern.
Update ownership totals: +E resources ROM_BASE_ONLY; the 387 pokedex flips
move 387 from COMPILED_PENDING to ROM_BASE_ONLY.

## 17. ROM-hack compatibility examples

| Mod | Mechanism | Class |
|---|---|---|
| change one trainer's party | override `emerald:data/trainer/<t>/party` | direct override |
| change one trainer's AI/metadata | override `emerald:data/trainer/<t>` | direct override |
| change Route 101 encounters | override `emerald:data/encounter/<route101>/land` (+ water/fishing) | direct override |
| change fishing on one map | override `emerald:data/encounter/<map>/fishing` | direct override |
| replace a Frontier trainer set | override `emerald:data/frontier/trainer/<t>` (+ mons) | direct override |
| replace a rental pool | override `emerald:data/frontier/factory/rental/<set>` | direct override |
| change one Pokédex entry | override `emerald:data/pokedex/<species>` (+ /category + /description text) | direct override |
| expand Pokédex text | MOD-provider overrides the pokedex text resources | direct override (schema unchanged; longer text is fine) |
| new trainer-party variant | new schema | requires expanded schema |
| new encounter mechanic | engine | mechanic adapter |
| new evolution/method | engine (D1 precedent) | mechanic adapter |

## 18. Subdivision decision

R13-E is broad; split by cutover/STOP/test boundary (not source directory):
- **E1 — trainers + parties + class names.** Independent live cutover
  (gTrainers + arena), independent parity STOP, clean test/isolation.
- **E2 — wild encounters.** Independent (gWildMonHeaders + info + slots);
  the revision-delta gate is its own STOP.
- **E3 — frontier + pokedex.** Grouped because both are text-re-point-heavy
  and irregular; each still has its own STOP condition. (Could further split
  E3a frontier / E3b pokedex if the frontier facility scope proves large.)
Each sub-stage: independent live cutover, independent STOP, clean test +
isolation boundary. Recommended order E1 → E2 → E3.

## 19. Suggested implementation order

1. Exact inventory + parity/revision-delta gate (this plan's agents; resolve
   all deltas to A/B/C/D before cutting manifests).
2. Vocabulary/generator extensions (trainer/encounter/frontier/pokedex
   families + any new schemas; cap constants).
3. Cap changes (pack-entry + merged-import + refusal-test pin → 32,768).
4. Pack rebuild (15,750 → ~19,000).
5. Lowest-risk structured family first: gTrainerClassNames / pokedex ordering
   (whole-table LEAF_BYTES, no pointer graph).
6. Pointer-graph families: trainer parties, encounters, frontier, pokedex
   text re-point.
7. R13-C text flips (pokedex 387).
8. State-v5 (family arena ranges; verify zero new sidecar records).
9. Isolation battery + ownership totals.
10. Battery + fresh builds + report.

## 20. Highest-risk family

**Wild encounters (`gWildMonHeaders` + WildPokemonInfo + slot rows).**
- Why: three unresolved revision deltas (125-vs-124, 209-vs-220, 2,070-vs-
  2,107) sit directly in this family; it has a 4-pointer-per-row header graph
  AND a 1-pointer-per-row info graph (two levels of indirection); it is the
  only core family whose count the current source and the ROM disagree on.
- Pointer graph: map → header (by mapGroup/mapNum) → land/water/fishing info
  ptr → slot table → species/level. A wrong alias/offset silently corrupts
  every encounter on a map.
- Revision-delta exposure: highest of any E family (three flags).
- State-v5 exposure: zero serialized pointers (transient), so failure is a
  runtime wrong-encounter, not a save-corruption — but that is still a
  silent-content regression.
- Stop gate that catches a bad migration: the revision-delta classification
  (must be A/B/C with C preserved, never D) + the whole-table + per-row
  provenance scan (ROM==pack==published byte equality) + the encounter
  header-pointer graph validation (every header's info pointer resolves to a
  declared info resource). Any unresolved delta = STOP the encounters family.

---

## Projected totals

| Quantity | Value |
|---|---|
| E resources (projected) | ~3,000–3,500 |
| E canonical bytes | ~175 KB (trainers 56.1K + encounters 16.7K + frontier 56.8K + pokedex 18.7K) |
| Pack after E | ~19,000–19,250 entries |
| Proposed new pack cap | **32,768** (covers E+F+G+H + remaining leaf wave) |
| Range count after E | ~5,860 / 8,192 (no raise) |
| R13-C text flips | 387 pokedex labels → ROM_BASE_ONLY |
| Subdivision | E1 trainers/parties · E2 encounters · E3 frontier+pokedex |
| Highest-risk family | wild encounters (3 revision deltas + 2-level pointer graph) |
| First implementation stage | E1 — trainers + parties + class names |

**STOP — R13-E is a design review; no production code was modified, no
commit made, R13-F not started. Final exact counts/bytes/parity verdicts are
filled from the inventory agents before implementation begins.**

---

## Implementation delta (R13-E1 — trainers, supersedes the E projections)

R13-E1 is **implemented and verified** (report:
`docs/R13E1_TRAINER_MIGRATION_REPORT.md`).

1. **Trainer parity: PASS (zero divergence)** — all 855 rows + 854 parties +
   66 classes byte-faithful to vanilla; no override needed. Corrections:
   `gTrainers` GBA wire 40 B/row (34,200 B; native 48 B); parties 18,088 B
   (not 15,066 — native size); `gTrainerMoneyTable` deferred (engine-adjacent).
2. **E1 resources:** 855 `emerald:data/trainer/<name>` + 854
   `…/party` + 66 `emerald:data/trainer-class/<cls>` = 1,775; gameplay+
   trainer total 5,462 / 411,964 B. Schemas 16/17/18.
3. **Pack:** 15,750 → **17,525 entries**, 13,595,744 B, SHA-256
   `9f8ca87b…6cb`, deterministic.
4. **Cap:** raised once to **32,768** (entry + merged-import + refusal pins);
   verified. Range cap 8,192 unchanged.
5. **Seam:** `EmeraldTrainerCompat` (transactional; party arena + native 48 B
   gTrainers rebuild + class rows); loader order play→gameplay→trainer;
   1 COMPAT_OBJECT range → post-E1 range index 5,848/8,192.
6. **Gates:** loader 62,993; trainer-compat 37,921; world 5,565; render
   3,628; import 155 — all pass. Release binary 22,363,584 → 22,442,568 B
   (+78,984 B). Compiled `sParty_*` absent; gTrainers/class-names are
   host_data fill targets.
7. E2 (wild encounters) is next; its four delta flags are resolved as
   non-divergences. F+E3 projections unchanged.

**R13-E1 STOP.** No commit made. E2/E3 and R13-F not started.

---

## Implementation delta (R13-E2 — wild encounters)

R13-E2 is **implemented and verified** (report:
`docs/R13E2_WILD_ENCOUNTER_MIGRATION_REPORT.md`).

1. **Inventory/re-deltas locked:** gWildMonHeaders 125 rows × 20 B = 124 real +
   1 MAP_UNDEFINED sentinel; 209 map-based info (95/55/6/53) + 209 slot tables
   / 1,975 rows / 7,900 B; zero aliasing. Deltas resolved: 125/124 =
   sentinel; 209/220 = +11 Pike/Pyramid → E3; 2,070/2,107 = stale arithmetic
   (1,975 map-based / 2,107 total). Parity PASS, no override.
2. **Resources:** `emerald:data/encounter/headers` (1 × 2,500 B) + 209
   `emerald:data/encounter/<map>/<type>` = **210 resources / 10,400 B**;
   schemas 19/20. Map keys = semantic MAP_* names (shared with R13-F);
   Altering Cave → altering-cave-1..9.
3. **Pack:** 17,525 → **17,735 entries**, 13,650,128 B, SHA-256
   `90a60c9d…aa6`, deterministic. Cap unchanged (32,768).
4. **Seam:** `EmeraldEncounterCompat` (transactional; 209/209 pointer-graph
   validation; slot arena + native gWildMonHeaders/infos rebuild); loader
   order …→trainer→encounter; 1 slot-arena range → post-E2 range 5,849/8,192.
5. **Gates:** loader 63,901; trainer 37,921; world 5,565; render 3,628 — all
   pass. Release 22,442,568 → 22,451,008 B (+8,440 B). Map-based slot arrays
   absent from the binary; Pike/Pyramid + sRoamerLocations stay compiled.
6. E3 (frontier+pokedex) inherits the deferred Pike/Pyramid tables + frontier
   pools + pokedex text.

**R13-E2 STOP.** No commit made. E3 and R13-F not started.

---

## Implementation deltas (R13-E3 — Frontier + Pokédex)

The E3 Frontier + Pokédex scope is **implemented and verified** across three
stages (reports: `R13E3A1_FRONTIER_TRAINER_GRAPH_REPORT.md`,
`R13E3A2_FRONTIER_AUX_MIGRATION_REPORT.md`, `R13E3B_POKEDEX_MIGRATION_REPORT.md`):

1. **E3a-1 (Frontier trainer/mon graph):** frontier trainers 300×52 + 300 mon-set
   leaves (28,060) + shared 882-mons pool (14,112 → 16/14 transform) + held
   items + banned species + Battle Tents (90 trainers/160 mons) = 786
   resources / 66,718 B.
2. **E3a-2 (facility aux + Pike/Pyramid wild):** factory move lists, palace/
   arena prizes, pike NPC (+ wild), pyramid floor/item (+ wild), brains,
   apprentice = 59 resources / 5,321 B; Pike/Pyramid wild handoff reused E2
   (+11 slot sets), no longer compiled.
3. **E3b (Pokédex):** 387 rows × 32 B GBA (category inline + 1 description ptr)
   + 4 ordering/routing tables = 391 resources; 387 description labels flipped
   ROM_BASE_ONLY (58,017 B).
4. Pack: 18,521 (E3a-1) → 18,580 (E3a-2) → **18,971 (E3b)** entries /
   13,987,040 B, SHA-256 `4e2be728…72e`. Cap unchanged.
5. Seams: EmeraldFrontierCompat (trainer/mon graph + aux) +
   EmeraldPokedexCompat; range index **5,851/8,192**; `gFacilityTrainers`/
   `gFacilityTrainerMons` relocated (E3a-1 State-v5 fresh-process gate green).
6. Battery (loader 65,658 / trainer 37,921 / world 5,565 / render 3,628) green.
   Release binary 22,488,384 → 22,442,296 B (net shrink −46 KB after E3b).
7. Engine-owned exceptions individually documented (dome, mindratings,
   fixed-IV/rental-range, pickup %, trainer-class music, brain flags/dialogue,
   apprentice dialogue). Remaining deferred: dex area/region graphics (gfx
   stage), map metadata (R13-F).

**R13-E STOP.** No commit made. R13-F not started.