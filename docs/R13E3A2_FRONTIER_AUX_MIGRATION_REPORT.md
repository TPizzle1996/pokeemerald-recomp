# R13-E3a-2 — Battle Frontier Facility Auxiliary + Pike/Pyramid Wild Handoff Report

Stage scope: migrate the Frontier facility auxiliary content (Factory/Palace/
Arena support, Pike NPC/content + wild handoff, Pyramid floor/item + wild
handoff, Frontier Brains, Apprentice) to ROM-owned resources by **extending**
`EmeraldFrontierCompat`. **No commit. E3b / R13-F not started.** E3a-1
trainer/mon graph untouched.

Status flags: ✅ done & verified.

---

## 1. Exact inventory (verified against the qualified ROM)

**E3a-2 = 59 resources / 5,321 B** (schemas 26–37, Generator 6,517 / 494,403 B
total):

| Facility | Content (migrate) | Resources | Canonical bytes | Class |
|---|---|---|---|---|
| Factory | 7 strategy move lists | 7 | 314 | LEAF_BYTES |
| Palace | early + late prizes | 2 | 30 | LEAF_BYTES |
| Arena | short + long prize items | 2 | 30 | LEAF_BYTES |
| Pike | NPC + speeches (11) | 12 | 1,019 | TRANSFORMED (NPC 25×8→6) / LEAF |
| Pyramid | floor (2) + items (deduped) + slots | 4 | 850 | TRANSFORMED (floor 16→13) / LEAF |
| Brains | ids + mons + shared streak | 3 | 882 | LEAF (mons 42×20) |
| Apprentice | gApprentices[16] | 16 | 1,408 | TRANSFORMED (88→86) |
| Wild headers | pike + pyramid header blocks | 2 | 260 | GBA_POINTER_GRAPH |
| Wild slots | 11 pike/pyramid sets | 11 | 528 | E2 schema (12×4 B) |

Inventory corrections vs review projections (ROM ground truth): Pike
`sNPCTable` = **25×8 B → native 6 B (150 B)** (not "50×4"); Frontier Brain
mons = **42 × 20 B** (not 12 B); `sPyramidFloorTemplateOptions` = 34×2 = 68 B;
pickup item pools deduped to one resource (lvl50 + lvlopen byte-identical).

## 2. Parity / revision-delta gate — PASS

Every E3a-2 symbol validated ROM-slice == ELF-slice at generation (full
sweep). Only GBA-pad vs native-pack (pike NPC, pyramid floor 16→13, apprentice
88→86) and pointer 4→8 layout differences; pickup-item twins + streak
duplication = bookkeeping (dedupe/repoint). No C/D divergence.

## 3. Pike/Pyramid handoff (reuse E2, mandatory)

`gBattlePikeWildMonHeaders` (5×20; 4 real + sentinel) +
`gBattlePyramidWildMonHeaders` (8×20; 7 real + sentinel) as whole-block
resources; 11 slot sets (pike-1..4 rate 10; pyramid-round-1..7 rates
4/4/4/4/4/4/8), 12×4 B each = 528 B. E2 WildPokemon schema + pointer-graph
validator proves every header→info→slot edge (11/11). Keys
`…/frontier/pike/wild/<set>` / `pyramid/wild/<set>`. The entire Pike/Pyramid
wild content is **no longer compiled** after the handoff.

## 4. Engine-vs-content boundary (retained, named, documented)

Dome; selection algorithms; streak calcs; state machines; callback dispatch;
`sMindRatings` (arena); `sFixedIVTable` + `sInitialRentalMonRanges` (factory);
`sBattlePalaceNatureToFlavorTextId`; `sBattledBrainBitFlags` +
`sFrontierBrainObjEventGfx` + the 6 brain text-pointer tables (frontier_util);
`sPickupItemOffsets` + `sPickupPercentages` + `sFloorTemplateOffsets` +
`sTrainerClassEncounterMusic` (pyramid); `sValidApprenticeMoves` +
`sInitialApprenticeIds` + apprentice dialogue. All stay compiled
(individually documented).

## 5. Resource keys

`…/frontier/factory/moves/<name>` · `palace/prizes/early|late` ·
`arena/prizes/short|long` · `pike/npc|speeches|…|wild/pike-<n>` ·
`pyramid/floor-templates|floor-options|items|item-slots|wild/pyramid-round-<n>`
· `brain/ids|mons|streak-appearances` · `apprentice/<index>`.

## 6. Publication (EmeraldFrontierCompat extension)

Extends the E3a-1 seam (`PublishFrontierAux`, no new seam): transactional
phase-1 validates every aux pool + the Pike/Pyramid edges + transform inputs;
phase-2/3 publishes native packed tables (NPC 8→6, floor 16→13, apprentice
88→86) + the shared 528 B wild slot arena + 11 native WildPokemonInfo +
brain/apprentice. 13 named subfamily error codes (ERR_PIKE_NPC,
ERR_APPRENTICE, ERR_PYRAMID_FLOOR, …). Atomic, REFUSE-class.

## 7. State-v5

E3a-1's `gFacilityTrainers`/`gFacilityTrainerMons` machinery unchanged; E3a-2
adds one COMPAT_OBJECT range over the wild-slot arena. Post-E3a-2 range index
= **5,851 / 8,192**. No format change, no cap raise.

## 8. Regression battery + builds ✅

**Builds:** `make -f Makefile_pc linux64` links (exit 0); `--verify-game-data`
exits 0 over the 18,580-entry pack.

**Battery (all green), 18,580-entry pack:** loader **65,658** (`TestFrontierAux`
+ refusals, GetPublishedCount 843); trainer **37,921**; world **5,565**;
render **3,628**.

**Pack:** 18,521 → **18,580 entries**, 13,897,808 B, SHA-256
`7add1b00e048dcda55bad634cd05a0936ec558101e8e303fc228ad497c84bfe1`,
deterministic rebuild. Cap unchanged (32,768).

**Binary (release):** E3a-1 22,478,800 → E3a-2 **22,488,384 B (+9,584 B)**.
**R13-C text flips:** 0 (brain/apprentice dialogue left as engine/script
indirection; target gText_* strings already R13-C-owned). **Isolation:** the
entire Pike/Pyramid wild content is no longer compiled (slot tables absent,
headers published as host_data fill targets); gApprentices/gBattlePikeNPC etc.
are host_data fill targets.

## 9. Manual gate (DINFO checklist)

1. Battle Factory; 2. Battle Palace; 3. Battle Arena; 4. Battle Pike;
5. Battle Pyramid; 6. Pike wild encounter; 7. Pyramid wild encounter;
8. Frontier Brain battle if practical; 9. save/load during a Frontier session;
10. no wrong pools/items/trainers/wilds/crash.

## 10. Blockers / prerequisites for E3b

- E3a-2 completes the Frontier lane (trainer/mon graph in E3a-1 + aux/wild in
  E3a-2). E3b (Pokédex) is independent; the 32,768 cap has ample headroom.
  No E3a-2 debt blocks E3b.

**STOP — R13-E3a-2 complete. No commit made. E3b / R13-F not started.**