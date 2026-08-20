# R13-E3a — Battle Frontier / Facility Structured-Content Migration Plan

Stage scope: migrate Battle Frontier / facility **structured original-game
content** (frontier trainers + mon sets + the shared mon pool, factory rental
support, dome/palace/arena/pike/pyramid facility content, frontier brains,
prize/item pools, and the Pike/Pyramid wild-encounter tables deferred from
E2) to ROM-owned resources via a new `EmeraldFrontierCompat` seam.
**Design/delta/parity review only — no production code modified, no commit.
E3b (Pokédex) and R13-F not started.**

Excluded: Pokédex (E3b), general map metadata (R13-F), field scripts (R13-G),
battle/AI script bytecode (R13-H), engine algorithms/RNG/callback tables
unless proven original payload.

Grounding: qualified reference ELF/ROM (`../pokeemerald-reference`, SHA-1
`f3ae0881…07b7`) + the R13-E-plan Frontier inventory agent + direct ROM
re-verification of the pointer-graph structures. Current proven state: pack
17,735 entries / 13,650,128 B; cap 32,768; ranges 5,849/8,192.

---

## 1. Exact Frontier inventory (ROM re-verified)

### Core shared pool (the Frontier's backbone)
| Family | Count × row | Canonical bytes | Native | Class |
|---|---|---|---|---|
| `gBattleFrontierTrainers` | 300 × 52 | **15,600** (@0x5d5acc) | 300 × 56 = 16,800 | GBA_POINTER_GRAPH (monSet ptr @48) |
| `gBattleFrontierTrainerMons_*` | 300 arrays (u16 idx, 0xFFFF-term) | **28,062** (contiguous 0x5ced2e..) | u16 leaves | INDEX_ROUTING leaf (per-trainer) |
| `gBattleFrontierMons` | 882 × 16 | **14,112** (@0x5d97bc) | 882 × 14 = 12,348 | TRANSFORMED_STRUCTURAL (shared indexed pool) |
| `gBattleFrontierHeldItems` | 63 × u16 | **126** | same | LEAF_BYTES |

`gBattleFrontierTrainers` row (52 B): facilityClass/trainerClass u8, name +
3×speech easy-chat u16 words, `const u16 *monSet` @48. **Text = embedded
easy-chat u16 words (not R13-C free text).** Mon-set leaf = u16 indices into
`gBattleFrontierMons`. `gBattleFrontierMons` row = species + moves[4] +
itemTableId + evSpread + nature (13 data bytes → 16 GBA / 14 native; agbcc
pad). **Zero aliasing** in the E-plan sample.

### Facility aux (original content)
| Facility | Content tables | Bytes | Class |
|---|---|---|---|
| Factory | `sMoves_*` 7 strategy lists × 56 | 392 | LEAF_BYTES (rental support); `sFixedIVTable`(16)+`sInitialRentalMonRanges`(64) ENGINE |
| Dome | — (runtime bracket/RNG) | 0 | ENGINE (nothing to extract) |
| Palace | `sBattlePalaceEarlyPrizes`(12)+`LatePrizes`(18) | 30 | LEAF_BYTES; `sBattlePalaceNatureToFlavorTextId`(25) ENGINE |
| Arena | `sShortStreakPrizeItems`(12)+`sLongStreakPrizeItems`(18) | 30 | LEAF_BYTES; `sMindRatings`(355) ENGINE |
| Pike | wild handoff (below) + `sNPCTable`(200 ROM/150 native)+`sNPCSpeeches`(504 easy-chat)+`sRoomTypeHints`(9)+`sNumMonsToHealBeforePikeQueen`(18) | ~731 | LEAF_BYTES / TRANSFORMED (NPC table) |
| Pyramid | wild handoff + `sPyramidFloorTemplates`(256 ROM/208 native)+options(68)+offsets(8) + item pools `sPickupItemsLvl50/Open`(400 each, byte-identical twins)+`sPickupItemSlots`(126)+offsets(7)+percentages(10) | ~1,175 | LEAF_BYTES / TRANSFORMED (floor templates) |
| Brains | `sFrontierBrainTrainerIds`(14)+`sFrontierBrainsMons`(840)+`sBattledBrainBitFlags`(28)+`sFrontierBrainObjEventGfx`(14); dialogue = text ptrs | 896 | LEAF_BYTES + text-ptr graph |
| Apprentice | `gApprentices[16]` | 1,408 (ROM 88 stride / 1,376 native 86) | TRANSFORMED_STRUCTURAL |
| Battle Tents | Slateport/Verdanturf/Fallarbor trainers+mons | ~5,360 ROM | same strides as main pool |
| Banned | `gFrontierBannedSpecies` (11 u16 + sentinel) | 22 | LEAF_BYTES |

### Pike/Pyramid wild-encounter handoff from E2 (re-verified)
| | info | slot rows | bytes | schema |
|---|---|---|---|---|
| Pike (`gBattlePikeWildMonHeaders`, 5 rows=4+sent) | 4 land | 48 (12×4) | 192 | WildPokemon (E2 slot schema) |
| Pyramid (`gBattlePyramidWildMonHeaders`, 8 rows=7+sent) | 7 land | 84 (12×7) | 336 | WildPokemon (E2 slot schema) |
| **total** | **11** | **132** | **528** | E2 reuse |

Rate: Pike 10; Pyramid 4 (rounds 1–6) / 8 (round 7). **Reuse the E2
encounter schema + pointer-graph validator — do NOT duplicate machinery.**
(The earlier Frontier-agent "sLvl50_Mons/PikeWildMon 36 B" reading was
imprecise; the ROM-verified structure is the WildPokemon slot tables above.)

## 2. Pike/Pyramid handoff model

Reuse E2: `emerald:data/frontier/pike/wild/<set>` and
`emerald:data/frontier/pyramid/wild/<set>` keyed per set (pike-1..4,
pyramid-round-1..7), payload = exact WildPokemon slot-table slice, rate as
metadata, header→info→slot edges validated by the E2-style validator. The
headers blocks (`gBattlePikeWildMonHeaders` 100 B / `gBattlePyramidWildMonHeaders`
160 B) are whole-block resources (like E2's `encounter/headers`).

## 3. Revision-delta gate (Frontier)

E-plan Frontier agent sampled: `gBattleFrontierTrainers` rows 0–9 data bytes
identical ROM↔native; `gBattleFrontierMons` row 0 identical; Brady mon-set
leaf identical. **No content deltas detected in sample.** Classification:
| Flag | Class | Resolution |
|---|---|---|
| agbcc 4-byte padding vs native packing (FacilityMon/PyramidFloorTemplate/PikeRoomNPC/ApprenticeTrainer) | B layout-only | seam remaps per-row |
| pointer growth 4→8 (trainer rows, pyramid ptr tables, brain text) | B layout-only | seam rebuilds |
| `sPickupItemsLvl50/LvlOpen` byte-identical twins | A bookkeeping | dedupe in catalog (one resource bound to both) |
| `sFrontierBrainStreakAppearances` duplicated (frontier_util.c + battle_pike.c) | A bookkeeping | one resource; both consumers repointed |
| `gBattlePyramidWildMonHeaders` in wild-encounter graph | B boundary | owned via E3a wild handoff, not double-owned |
| (full-table sweep) | — | required during implementation; any D-class = STOP |

No deliberate fork behavior delta observed; if the implementation sweep finds
one, preserve it explicitly (never normalize).

## 4. Frontier trainer resource identity

```
emerald:data/frontier/trainer/<n>          (n = 0..299 trainer id)
emerald:data/frontier/trainer/<n>/mons     (mon-set leaf, u16 indices)
emerald:data/frontier/mons                 (shared 882-pool, indexed)
emerald:data/frontier/held-items
emerald:data/frontier/brain/...            (distinct brain namespace)
```
Per-trainer metadata + per-trainer mon-set → a mod replaces one trainer / its
mon pool without replacing all Frontier data. The shared `gBattleFrontierMons`
is an indexed pool → one whole-table resource (not 882 per-mon). Brains use a
distinct namespace. Trainer names/speech = embedded easy-chat words (stay
in-row; not R13-C text).

## 5. gBattleFrontierTrainers pointer graph + publication

Analogous to E1 trainers: canonical trainer row (52 B, monSet ptr retained as
validated provenance) + mon-set leaf → native HOST_DATA trainer row + native
mon-arena pointer. No native pointers in pack. Consumer logic unchanged
(`gFacilityTrainers` alias + `GetFrontierTrainerMonSet`-style access).

## 6. Frontier mon-set schemas

Mon-set leaves = u16 index arrays (INDEX_ROUTING leaf; single schema).
`gBattleFrontierMons` row = FacilityMon (species/moves/itemTableId/evSpread/
nature) — one schema. **Do NOT normalize into a generic Pokémon struct** —
canonical payloads stay exact ROM slices.

## 7. Facility-pool granularity

- Frontier trainers/mon-sets: **PER_ENTITY** (300 + 300).
- `gBattleFrontierMons`, held items, banned species: **WHOLE_TABLE** (shared
  indexed pools).
- Factory/Palace/Arena/Pyramid prize + item pools, Pike NPC/speeches:
  **PER_POOL** (one resource per named pool).
- Dome: **ENGINE_CONSTANT** (nothing to extract).
- `sMindRatings`, streak/range/IV tables, nature-flavor: **ENGINE_CONSTANT**.

## 8. Engine-vs-content boundary (critical)

- **CONTENT** (migrate): frontier trainers, mon sets, shared mon pool, held
  items, factory move-strategy lists, palace/arena prize tables, pike NPC +
  speeches + wild, pyramid floor templates + item pools + wild, brain mons +
  trainer ids, apprentice data, battle tents, banned species.
- **ENGINE** (stay compiled): dome bracket/RNG, streak calculations, selection
  algorithms, AI code, facility state machines, code-pointer dispatch
  (`sBattleDomeFunctions`, palace nature-flavor, arena `sMindRatings`,
  factory IV/rental-range tables, pyramid pickup percentages/offsets, trainer-
  class encounter music).
- **INDEX_ROUTING** (may stay compiled if ID-mapping only): trainer-id range
  tables, brain-streak appearances.
Do NOT migrate engine behavior just because it sits in a data source file.

## 9. Text interaction

Frontier trainer name/speech = embedded easy-chat u16 words (in-row; not
R13-C free-text). Brain dialogue = pointer tables to `gText_*` strings →
check which are R13-C-owned vs deferred. Apprentice dialogue partially
text-owned. **Only flip the deferred R13-C text resources whose owning table
lands in E3a** (exact count to be pinned during implementation from
`emerald:text/…` bindings that brain/apprentice tables target). Do NOT pull
script dialogue (R13-G).

## 10. Function-pointer / callback fields

Frontier data rows contain no native code pointers in the content tables
(callbacks live in engine dispatch tables like `sBattleDomeFunctions`, which
stay ENGINE_CONSTANT). Brain dialogue pointers are text pointers (§9), not
code. If a callback registry is ever needed, model it after R13-D2 items — but
none is required for E3a content.

## 11. State-v5

The two serialized Frontier pointers are `gFacilityTrainers` +
`gFacilityTrainerMons` (EWRAM globals, `battle_tower.c:44`, assigned in
`SetFacilityPtrsGetLevel`). After E3a they point into the published frontier
trainer/mon arenas. Register family/arena ranges over the published frontier
trainer + mon arenas (frontier trainer table, mon arena; + tent aliases if
they share). **Projected post-E3a range count: ~5,852** (5,849 + up to 3
frontier arena ranges). No per-resource ranges, no format change, no cap
raise. Add a fail-closed fresh-process relocation test for a frontier pointer.

## 12. Native publication architecture

One **`EmeraldFrontierCompat`** seam with internal subfamilies (transactional;
subfamily failures named precisely; facility arrays committed atomically),
reusing the E2 encounter validator for the Pike/Pyramid wild handoff.
Publication order: (1) shared mon pool + held items, (2) frontier trainers +
mon sets, (3) battle tents, (4) facility aux pools, (5) Pike/Pyramid wild
handoff, (6) brains + apprentice.

## 13. Live-cutover classification

| Family | LIVE after E3a? | Compiled removed? | Transformed? | Text re-point? | Engine exception? |
|---|---|---|---|---|---|
| frontier trainers | yes | yes | yes | no (easy-chat in-row) | no |
| mon sets | yes | yes | no (u16 leaves) | no | no |
| shared mon pool | yes | yes | yes | no | no |
| held items / banned | yes | yes | no | no | no |
| factory lists | yes | yes | no | no | fixedIV/rental-range ENGINE |
| palace/arena prizes | yes | yes | no | no | nature-flavor/mindratings ENGINE |
| pike npc/speeches/wild | yes | yes | npc yes | easy-chat in-row | room hints ENGINE |
| pyramid floor/item/wild | yes | yes | floor yes | no | pickup % ENGINE |
| brains | yes | yes | no | brain dialogue TBD | streak appearances ENGINE |
| apprentice | yes | yes | yes | partial | no |
| battle tents | yes | yes | yes | no | no |
| dome | n/a | stays | — | — | ENGINE (nothing) |

No vague "partial Frontier" — exactly what stays compiled is enumerated above.

## 14. Isolation

Symbol absence for frontier trainer rows + mon-set leaves (leaf-symbol sweep);
table hashes for the shared mon pool + facility pools; pointer-edge validation
(trainer.monSet→leaf; leaf idx→mons pool); exact short-row exemptions (prize
tables 12–18 B). ENGINE_CONSTANT families individually documented. No blanket
Frontier exemption.

## 15. Pack arithmetic

Current 17,735 / cap 32,768. E3a projection (approximate; exact count pinned
during implementation):
- trainers+monsets ~600, shared mons/held/banned ~3, factory ~7, palace/arena
  ~4, pike ~6, pyramid ~8, brains ~4, apprentice 1–16, tents ~6 → **~650
  resources**, canonical ~70–80 KB (trainer/mon 43,662 + mons 14,112 + pools +
  Pike/Pyramid wild 528 + misc).
- Projected pack ≈ **18,380–18,450 entries** (no Pokédex E3b). Well under cap.
No cap change.

## 16. ROM-hack compatibility

| Mod | Mechanism | Class |
|---|---|---|
| replace one Frontier trainer | override `…/frontier/trainer/<n>` | direct override |
| replace one trainer's mon set | override `…/frontier/trainer/<n>/mons` | direct override |
| replace Factory rental pool | override factory pool resources / shared mons | direct (or shared-pool override) |
| change Pike/Pyramid wild mons | override `…/frontier/pike|pyramid/wild/<set>` | direct override |
| change BP prize table | override prize pool resource | direct override |
| change Frontier Brain team | override `…/frontier/brain/…` | direct override |
| new facility mechanic / new mon schema | engine / schema | mechanic adapter / schema extension |

## 17. E3a subdivision check

**Recommend a split:**
- **E3a-1 — frontier trainer + mon graph:** gBattleFrontierTrainers + mon sets
  + shared mon pool + held items + battle tents. This is the pointer-graph +
  State-v5 (gFacilityTrainers/gFacilityTrainerMons) core. Independent live
  cutover + STOP + isolation.
- **E3a-2 — facility aux + Pike/Pyramid wild/prize pools:** factory/palace/
  arena/pike-npc/pyramid-floor/prizes/brains/apprentice + the E2 wild handoff.
  Independent live cutover + STOP + isolation.
Each has an independent cutover/STOP/isolation boundary; the split keeps each
bounded. If implementation shows E3a-2 is trivial, merge into one stage.

## 18. Highest-risk Frontier family

**The frontier trainer pointer graph + State-v5 alias** (`gBattleFrontierTrainers`
→ monSet → `gBattleFrontierMons`, surfaced through the serialized
`gFacilityTrainers`/`gFacilityTrainerMons` EWRAM pointers).
- Risk: a wrong mon-set/pool edge or a broken facility-pointer repoint corrupts
  every facility battle AND the mid-frontier save (the two serialized pointers).
- STOP gate: 300/300 trainer.monSet→leaf edges + every mon-set index resolves
  into the shared pool (0 out-of-range) + `gFacilityTrainers`/
  `gFacilityTrainerMons` repoint to the published arenas + a fresh-process
  frontier-save relocation test.
- Test: publish, battle a frontier trainer, save mid-facility, reload in a
  fresh process, verify the facility pointers resolve + the battle continues.

---

## Projected totals

| Quantity | Value |
|---|---|
| frontier trainers / mon-sets | ~600 resources (15,600 + 28,062 B) |
| shared mon pool / held / banned | ~3 resources (~14,260 B) |
| facility aux pools | ~25 resources |
| Pike/Pyramid wild handoff | 2 header blocks + 11 info + slot tables (528 B slots) |
| brains / apprentice / tents | ~12 resources |
| E3a resources (approx) | **~650** / ~70–80 KB |
| projected pack | **~18,380–18,450** entries (no cap change) |
| State-v5 | ~+3 arena ranges → ~5,852/8,192 |
| split | **E3a-1 trainer/mon graph · E3a-2 facility aux + wild handoff** |
| highest-risk | frontier trainer pointer graph + facility save pointers |
| complexity | medium-high (irregular facility content + State-v5 alias) |

**STOP — R13-E3a is a design/delta/parity review; no production code modified,
no commit made, E3b/R13-F not started.**

---

## Implementation delta (R13-E3a-1 — Frontier trainer + mon graph)

R13-E3a-1 is **implemented and verified** (report:
`docs/R13E3A1_FRONTIER_TRAINER_GRAPH_REPORT.md`).

1. **Inventory:** frontier trainers 300×52 (15,600) + 300 mon-set leaves
   (28,060) + shared 882-mons pool (14,112) + held items (126) + banned
   (22) + Battle Tents (90 trainers + 160 mons = 183 pools/leaves). Tent
   model confirmed = index-stream (monSet → 0xFFFF-term u16 stream into the
   tent's own pool). **E3a-1 = 786 resources / 66,718 B.**
2. **Parity:** full (not sampled); generator ROM==ELF per resource; seam
   byte-matches. All 390 trainer→mon-set edges valid; every mon index in
   range (0..881, in-pool tents).
3. **Pack:** 17,735 → **18,521 entries**, 13,880,208 B, SHA-256
   `ea0523ee…20e`, deterministic. Cap unchanged.
4. **Seam:** `EmeraldFrontierCompat` (transactional; mon-set arena + 16→14
   mons transform + rebuilt monSet pointers + tent tables). Loader order
   …→encounter→frontier. State-v5: one mon-set-arena range → 5,850/8,192;
   `gFacilityTrainers`/`gFacilityTrainerMons` re-targeted (fresh-process
   relocation test green).
5. **Gates:** loader 65,597; trainer 37,921; world 5,565; render 3,628 — all
   pass. Release 22,451,008 → 22,478,800 B (+27,792 B). Frontier tables are
   host_data fill targets; compiled mon-set/tent leaves absent.
6. E3a-2 (facility aux + Pike/Pyramid wild handoff + brains/apprentice) and
   E3b (pokedex) are unchanged and not started.

**R13-E3a-1 STOP.** No commit made. E3a-2 / E3b / R13-F not started.

---

## Implementation delta (R13-E3a-2 — facility aux + Pike/Pyramid wild)

R13-E3a-2 is **implemented and verified** (report:
`docs/R13E3A2_FRONTIER_AUX_MIGRATION_REPORT.md`).

1. **Inventory:** 59 resources / 5,321 B — factory move lists (7), palace/
   arena prizes (4), pike NPC+speeches (12), pyramid floor/items/slots (4),
   brains (3), apprentice (16), Pike/Pyramid wild headers (2) + 11 slot sets.
   Corrections: pike NPC 25×8→6 (150 B), brain mons 42×20, pickup-item twins
   deduped. Total gameplay+frontier = 6,517 / 494,403 B.
2. **Pike/Pyramid handoff:** reused E2 WildPokemon schema + validator
   (11/11 edges); the entire wild content is no longer compiled.
3. **Pack:** 18,521 → **18,580 entries**, 13,897,808 B, SHA-256
   `7add1b00…bfe1`, deterministic. Cap unchanged.
4. **Seam:** `EmeraldFrontierCompat` extended (`PublishFrontierAux`, 13 named
   subfamily error codes); loads …→encounter→frontier. State-v5: +1 wild-slot
   arena range → 5,851/8,192.
5. **Gates:** loader 65,658; trainer 37,921; world 5,565; render 3,628 — all
   pass. Release 22,478,800 → 22,488,384 B (+9,584 B). R13-C text flips = 0
   (brain/apprentice dialogue stays as script/engine indirection).
6. E3a-2 completes the Frontier lane. E3b (pokedex) and R13-F are unchanged
   and not started.

**R13-E3a-2 STOP.** No commit made. E3b / R13-F not started.