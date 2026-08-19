# R13-D1 — Gameplay Data + content.pak Absorption Report

Stage scope: species / move / shared gameplay tables / fonts publication and
the **complete retirement of the legacy content.pak**. D2 (gItems + callback
registry + item text cutover) is out of scope. R13-E not started. **No commit
made for this stage.**

Status flags: ✅ done & verified · 🅿 pending (none after the implementation +
battery pass — all gates recorded below).

---

## 1. D1 inventory (pinned; corrected by the parity gate)

All counts re-derived from the qualified reference ELF
(`../pokeemerald-reference/pokeemerald.elf`) + retail BPEE01 ROM (SHA-1
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`) and verified byte-for-byte
(`ELF == ROM == pack == published`).

**Pack-entry arithmetic (D1):** 12,063 (R13-C) + 3,310 (D1) = **15,373**.
The plan's 16,162 (D1+D2) projection falls to **15,750** because the
evolution family is excluded (below).

| Family | Schema | Resources | Canonical bytes (GBA wire) |
|---|---|---|---|
| species base | 1 | 412 × 28 B | 11,536 |
| species names | 2 | 412 × 11 B | 4,532 |
| level-up learnset leaves | 4 | 411 (distinct; index 0 shares bulbasaur's leaf) | 8,766 |
| species TM/HM | 5 | 412 × 8 B | 3,296 |
| species tutor | 6 | 412 × 4 B | 1,648 |
| species egg moves | 7 | 165 blocks (+ terminator) | 2,278 |
| move battle | 8 | 355 × 12 B | 4,260 |
| move names | 9 | 355 × 13 B | 4,615 |
| move contest | 10 | 355 × 8 B | 2,840 |
| growth rates | 12 | 8 × 404 B | 3,232 |
| tutor move list | 13 | 1 × 60 B | 60 |
| contest effects | 14 | 1 × 192 B | 192 |
| contest combo starters | 15 | 1 × 63 B | 63 |
| **structured subtotal** | | **3,300** | **47,318** |
| fonts (type FONT, schema 1) | | 10 (8×32,768 + 2×16,384) | 294,912 |
| **D1 total** | | **3,310** | **342,230** |

*Correction vs plan:* the plan pinned 3,722 / 358,710 B (incl. 412 evolution
resources / 16,480 B). The evolution family is excluded — see §2.

**Production pack after D1:**

| Quantity | Value |
|---|---|
| Entry count | **15,373** |
| Byte size | **13,102,112 B** |
| SHA-256 | `d2f0e03313cb93bc3fee3747a061e60000ab9c0bd17b5068527a464adebdf013` |
| Deterministic rebuild | ✅ byte-identical (`gen3-pack-build --check`) |
| Cap | 15,373 ≤ 16,384 (no raise in D1) |

---

## 2. Compile-vs-vanilla parity gate (mandatory pre-cutover)

Every still-compiled D1 family's native bytes were compared to the vanilla
ROM slice, normalized to GBA wire (pad expansion where the native struct is
packed). Verdict:

| Family | Result |
|---|---|
| level-up learnset leaves | ✅ PASS (412/412 leaf byte-identical) |
| gTMHMLearnsets | ✅ PASS |
| sTutorLearnsets / gTutorMoves | ✅ PASS |
| gEggMoves | ✅ PASS |
| gContestMoves (7→8) / gContestEffects (3→4) | ✅ PASS |
| gComboStarterLookupTable | ✅ PASS — **ground truth 63 B**; recomp compiled object is 63 B; first 63 bytes equal the ROM; **no byte[63] on either side** (the long-flagged 64-vs-63 resolves as a non-divergence) |
| gEvolutionTable | ❌ **DIVERGE (STOPped)** |

**gEvolutionTable STOP (per R13-D brief §2/§22):** byte-parity proved the
recomp **deliberately replaced the 12 vanilla trade evolutions** with
item / level-40 evolutions (`src/data/pokemon/evolution.h:43,45`, e.g.
`{EVO_LEVEL, 40, SPECIES_ALAKAZAM}` / `{EVO_LEVEL, 40, SPECIES_MACHAMP}`
where vanilla uses `EVO_TRADE`; Poliwhirl/Slowpoke/Onix/Seadra/Scyther/Porygon/
Clamperl use `EVO_ITEM` instead of `EVO_TRADE_ITEM`; targets and item params
are identical). Publishing vanilla evolution data would silently overwrite
this fork behaviour — forbidden. **The evolution family is therefore NOT
migrated in D1; `gEvolutionTable` stays compiled and is a documented
behavioral-divergence exemption.** This is the proven correction to the 3,722
pin. (All 12 diverging rows share target+param; only the method byte and the
level-40 dummy param differ; the 8-byte pad is zero on both sides.)

Everything else passes, so D1 ships every parity-passing family.

---

## 3. Transforms (canonical → native, seam-owned)

Canonical pack payloads are **exact padded GBA wire rows**; the native
packed structs are produced by the publication seam only (never stored in the
pack):

| Family | Canonical wire | Native struct |
|---|---|---|
| species base | 28 B | `struct SpeciesInfo` 26 B (reuses the verified content.pak `HydrateEntry` field layout, incl. EV-yield bitfield unpack and bodyColor/noFlip split) |
| move battle | 12 B | `struct BattleMove` 9 B |
| move contest | 8 B | `struct ContestMove` 7 B |
| contest effects | 4 B | `struct ContestEffect` 3 B |
| names / TMHM / tutor / tutor-moves / egg / growth / combo | raw | raw copy |
| learnset leaves | u16 streams | arena + rebuilt `gLevelUpLearnsets` pointer table |
| fonts | glyph wire | u16 LE word loads |

Resource identity keys, per the approved plan:
`emerald:data/species/<name>[/name|/levelup|/tmhm|/tutor|/egg-moves]`,
`emerald:data/move/<name>[/name|/contest]`,
`emerald:data/growth-rate/<rate>`, `emerald:data/tutor/moves`,
`emerald:data/contest/effects`, `emerald:data/contest/combo-starters`,
`emerald:font/<font>`. No `.../evolutions` keys are emitted.

---

## 4. Vocabulary & generator

- `GEN3_PACK_TYPE_STRUCTURED_DATA = 16` (+ host
  `GEN3_RESOURCE_TYPE_STRUCTURED_DATA`), manifest/importer/validator pairs
  (structured-data, gba-bytes) and (font, gba-bytes) fail closed.
- `tools/gen3_resources/gameplay_family/gen_gameplay_family.py`: emits
  inventory / catalog / bindings / ownership / consumers / production
  manifest (direct, per-row `rom_offset`) / 3,310 row artifacts / the native
  seam inventory + index maps. **`--check` is a byte-identical no-op.**
- Generated: `gameplay_native.generated.{h,c}` (3310-row inventory),
  `gameplay_levelup.generated.c` (species/move base key maps +
  species→levelup key map).

---
## 5. content.pak retirement ✅

- Old flow retired: `Platform_GameContentImport` no longer builds content.pak;
  `VerifyInstalled` no longer hydrates the five tables or ten fonts from
  content.pak; the EMRLDATA format, `sCanonicalEntries`, the pak writer/reader
  and manifest.json handling are removed (source-grep clean: no `content.pak`,
  `EMRLDATA`, `HydrateEntry`, or `sCanonicalEntries` remain in `src/`).
- The five previously-hydrated `HOST_DATA` arrays (gSpeciesInfo, gBattleMoves,
  gExperienceTables, gSpeciesNames, gMoveNames) now live in
  `src/emerald/resources/gameplay_data_native.c` and are filled by the seam.
- New single flow: verified ROM → `EmeraldImport_ValidateRom`/`BuildPack`/
  `Install` (10+10 merged manifest/catalog set, same as `build_d1_pack.sh`) →
  installed .rpack → `RegisterRuntimeSnapshot` → `TryInitialize` (all seams
  incl. gameplay) → `NativeWorldNeighborhood_Init` → `AgbMain`.
- `VerifyInstalled(hydrate)` verifies the installed .rpack and (when TRUE) runs
  the registration block; boot call sites (`sdl2.c`, `desktop_frontend.c`)
  keep their signatures, so the frontend/CLI surface is unchanged.
- Old content.pak may remain inert on disk but is never read; it cannot rescue
  a missing/broken rpack (refusal semantics preserved — the runtime-loader
  battery exercises these refusals).

## 6. Live family status ✅

All D1 structured families + fonts are **ROM_BASE_ONLY** (pack authoritative);
the compiled originals are guarded out of the native build via a
`NATIVE_LINUX` guard split (extern + HOST_DATA fill target on native, `const`
definition preserved for GBA) for: levelup leaves + pointer table, TM/HM,
tutor (+ move list), egg moves, contest moves/effects, combo starters. The
five previously-hydrated families keep their array symbols as fill targets.
Consumer-identical: zero mass call-site edits across `gSpeciesInfo`
(144 sites), `gBattleMoves` (193), names, learnsets, TMHM, tutor, egg, contest.

## 7. State-v5 ✅

No format change; no new sidecar record class. **12** COMPAT_OBJECT ranges
registered (keys `emerald:data/arena/<family>`: levelup leaf arena (schema 4),
egg array (schema 7), 10 font glyph arrays (type FONT)); **no per-row
ranges**. Range index at **5,847 / 8,192** (verified in the loader battery).

## 8. Isolation + intentionally retained families ✅

- Retained compiled (documented, NOT blanket-exempted): `gEvolutionTable`
  (behavioral divergence, §2), `gItemEffectTable` + leaves (fork debt, D2
  era), `gItemIconTable` + icon gfx (gfx-leaf wave), routing tables
  (gSpeciesIdToCryId, sSpeciesToNationalPokedexNum, gPokedexOrder_*),
  `gContestEffectFuncs` + ItemUse engine callbacks, font width tables +
  bold/braille/keypad glyph residue.
- Isolation: the D1 seam validates source/ELF == ROM == pack == published form
  (phase-1 composition/size/ownership/disjointness + transform-oracle equality
  at fill); fill targets present as HOST_DATA, const payloads guarded out;
  `--verify-game-data` + the loader battery exercise the full chain over the
  15,373-entry pack.

## 9. Builds, battery, binary delta, manual gate

**Builds:** native Linux64 links and runs. `--verify-game-data` over the
15,373-entry production pack exits 0 (the full loader chain —
trainer/audio/leaf/text/gameplay seams — publishes and verifies).

**Regression battery (all green):**

| Suite | Checks | Result |
|---|---|---|
| `run_emerald_runtime_loader.sh` | 62,934 | ✅ PASS (incl. new `TestGameplayPublication`, D1 range index 5,847/8,192, content.pak refusal + composition failure paths) |
| `run_emerald_trainer_native_compat_production.sh` | 37,921 | ✅ PASS (15,373-pack catalog + ROM_BASE snapshot, gameplay catalog wired) |
| `run_emerald_native_world_real.sh` | 5,565 | ✅ PASS |
| `run_emerald_native_world_render_proof.sh` | 3,628 | ✅ PASS |

**Binary delta (release):** 22,176,784 B (R13-C fresh release) →
**22,368,088 B** = **+191,304 B (+0.19 MB)** — the R13-D1 seam + generated
inventory + fill targets, net of the compiled payloads removed (learnset
leaves + pointer table, TMHM, tutor, egg, contest, and the hydrated arrays'
compiled backing). Note: a separate DINFO-flavor delta was not captured in
this pass.

**Pack (final):** 15,373 entries / 13,102,112 B / SHA-256
`d2f0e03313cb93bc3fee3747a061e60000ab9c0bd17b5068527a464adebdf013` /
deterministic rebuild byte-identical.

**Manual gate (DINFO checklist):** 1) boot from a valid installed .rpack with
no content.pak dependency; 2) Pokémon summary / stat screens; 3) leveling +
evolution (item/level-40 on the recomp's reworked set); 4) learn a level-up
move / relearner; 5) TM/HM compatibility; 6) move names + values in battle;
7) contest move data; 8) font rendering across menus + dialogue; 9) save /
restart / reload; 10) no wrong stats/moves/text/glyph corruption.

## 10. Prerequisites for D2

- gameplay vocabulary (STRUCTURED_DATA schema 11 = item) already permitted by
  the manifest validator.
- D1 seam/generator patterns (transform + species/move index maps) extend
  trivially to the gItems 44-B rows + 27-entry callback registry + R13-C
  item-text re-pointing.
- No D2 item work was begun here.

**STOP — R13-D1 complete. No commit made. D2 and R13-E have not been
started.**