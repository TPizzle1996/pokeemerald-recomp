# R13-H1 — Battle Script Graph Report

Status: **complete**. This was an analysis / generation / oracle stage only.
No live battle/animation/AI execution was modified, no H arenas were
created, no H resources were published, no State-v5 ranges were registered,
no runtime behavior was changed, nothing was committed.

All pins below are reproduced from the qualified BPEE01 ROM
(`f3ae088181bf583e55daf962a92bb46f4f1d07b7`), the qualified linked ELF, and
the qualified per-family object files (`../pokeemerald-reference/
build/emerald/data/*.o`, relocation tables intact), independently through a
three-way static oracle: ROM raw == ELF relocation result == generated
record. Where a plan baseline differed from the qualified truth, the truth
is documented below and the baseline corrected — the old number was not
forced.

Generated artifacts (10 TOML files + 5 grammar fixtures + debug JSON) live
under `resources/extraction/emerald/bpee01/battle/`; all are `--check`
byte-identical on regeneration.

## 1. Family inventories (brief §1)

| Family | Object | Base (GBA) | Bytes | Spans | Instructions | Routing rows | Relocs |
|---|---:|---:|---:|---:|---:|---:|---:|
| Battle scripts | battle_scripts_1.o | 0x082d86a8 | 13,099 | 619 | 3,275 | 214 | 1,508 |
| Battle item-routing | battle_scripts_2.o | 0x082dbd08 | 493 | 26 | 115 | 24 | 54 |
| Battle anim scripts | battle_anim_scripts.o | 0x082c8d64 | 63,811 | 658 | 9,215 | 399 | 4,231 |
| Battle AI scripts | battle_ai_scripts.o | 0x082dbef8 | 9,303 | 553 | 1,738 | 32 | 1,222 |
| Contest AI scripts | contest_ai_scripts.o | 0x082de350 | 2,524 | 165 | 613 | 32 | 364 |
| Field-effect scripts | field_effect_scripts.o | 0x082db9d4 | 817 | 68 | 137 | 67 | 170 |
| **H bytecode total** | | | **90,047** | **2,089** | **15,093** | **768** | **7,549** |

Reloc counts are exact for every family (1,562 / 4,231 / 1,222 / 364 / 170
— all match the plan). Byte totals and span counts differ from the plan's
§1.1 estimate by inter-object alignment padding, as reconciled below.

**Baseline corrections (plan §1.1 → qualified truth).** The plan's byte
figures were address-gap widths (including trailing `.balign` padding before
the next object); the qualified `sh_size` payloads are the authoritative
canonical bytes:

| Family | Plan bytes | H1 bytes | Delta | Plan spans | H1 spans |
|---|---:|---:|---:|---:|---:|
| battle_scripts_1 | 13,100 | 13,099 | −1 | 617+26+1 labels | 619 (613 script + 1 routing + 5 empty) |
| battle_scripts_2 | 496 | 493 | −3 | 22+4 labels | 26 (22 script + 4 routing) |
| battle anim | 63,812 | 63,811 | −1 | ≈657 labels | 658 (650 script + 5 routing + 3 empty) |
| battle AI | 9,303 | 9,303 | 0 | 533+22 labels | 553 (527 script + 25 data + 1 routing) |
| contest AI | 2,524 | 2,524 | 0 | 164+1 labels | 165 (164 script + 1 routing) |
| field effect | 820 | 817 | −3 | 67 scripts | 68 (67 script + 1 routing) |
| **Total** | **90,055** | **90,047** | **−8** | | **2,089** |

The −8 B is padding only (1 + 3 + 1 + 3 across the four padded objects).
Span counts include routing tables, empty alignment spans, and (AI) the 25
typed data spans; label counts from the source files exceed span counts
where multiple labels share one span (aliases). The plan's §18 canonical
bytes "≈90.1 KB (90,055)" is corrected to **90,047 B**.

## 2. Grammars (brief §2, §9, §23)

Machine-readable grammars were generated from the **qualified** macro files
(the recomp repo's `asm/macros/*.inc` are a newer pret revision) and
cross-checked against the C interpreter tables:

| VM | Table | Encodings | Unique opcodes | Handler entries | Size-aliased opcodes |
|---|---:|---:|---:|---:|---|
| Battle | gBattleScriptingCommandsTable | 300 | **249** | 249 | 46 (10 encodings: 6/9/12/24 B; setword = 4× setbyte) |
| Battle anim | sScriptCmdTable | 51 | **48** | 48 | none |
| Battle AI | sBattleAICmdTable | 114 | **99** | 99 | 76, 95, 96 (query/if/if-not triples) |
| Contest AI | sContestAICmdTable | 140 | **136** | 136 | none |
| Field effect | gFieldEffectScriptFuncs | 8 | **8** | 8 | none |

Every opcode in the C table range is emitted (missing = 0, extra = 0). The
battle 249-opcode table has no untyped slots: every operand is assigned one
of the typed classes below; operand widths come from the macro emission
directives, and access width from the **handler semantics** (e.g. `setword`
is a macro expanding to 4× `setbyte`, so its engine target is accessed at
u8 width — never inferred from the operand slot's 4-byte width). Pointer-ness
is never inferred from numeric value shape: a record is a pointer only when
it is a relocation site; the 65 stored-zero u32 slots with **no** relocation
are NULL sentinels (§5). Fixtures: `h1_grammar_{battle,battle_anim,
battle_ai,contest_ai,field_effect}.generated.toml`.

## 3. Relocation census and closure (brief §3, §22)

Every relocation in the six objects is classified into one of six typed
classes and attributed to exactly one owner — an operand slot of a command,
a routing-table row, or a data-table row:

| Class | bs1 | bs2 | anim | AI | contest | FE | Total |
|---|---:|---:|---:|---:|---:|---:|---:|
| SCRIPT_TARGET | 962 | 31 | 1,152 | 1,222 | 364 | 67 | **3,798** |
| ENGINE_EWRAM_TARGET | 472 | 16 | 0 | 0 | 0 | 0 | **488** |
| ENGINE_TABLE_TARGET | 74 | 7 | 8 | 0 | 0 | 0 | **89** |
| ENGINE_SPRITE_TEMPLATE_TARGET | 0 | 0 | 2,108 | 0 | 0 | 0 | **2,108** |
| ENGINE_CALLBACK | 0 | 0 | 963 | 0 | 0 | 67 | **1,030** |
| ENGINE_GFX_TARGET | 0 | 0 | 0 | 0 | 0 | 36 | **36** |
| **Total** | 1,508 | 54 | 4,231 | 1,222 | 364 | 170 | **7,549** |

Closure: **universe = included = 7,549; excluded = 0, remainder = 0,
unresolved = 0, ambiguous = 0, unknown = 0.** There are no explicit
exclusions and no UNKNOWN class; every relocation has a named owner.

**Corrections vs plan §1.2:** SCRIPT_TARGET ≈3,728 → **3,798** (+70; the
plan's estimate undercounted section-local command targets); EWRAM ≈480 →
**488** (472+16), 23 unique symbols (vs ≈24); anim CALLBACK 962 → **963**
(213 unique symbols vs 212; 280 unique incl. FE's 67 → 1,030 occurrences);
FE gfx ≈40 → **36** and FE callnative ≈63 → **67** (total 103 exact); the
plan's "≈14 residual constant/misc relocations" in the battle row is **0**
after exact classification — the battle row sums exactly (993 SCRIPT + 488
EWRAM + 81 TABLE = 1,562) — and the anim "6 global + 3 misc" residual is
exactly **8 gIceCrystalSpiralInward{Small,Large} table targets** (4 each,
`createsprite`) + 1 callback occurrence.

## 4. Engine/EWRAM binding census (brief §4 — hard gate)

718 generated semantic binding rows, one per unique (gba_base_symbol,
addend), each carrying the full §4 schema: `gba_base_symbol`,
`encoded_gba_value`, `addend`, `native_binding_symbol`, `allowed_offset`
(ELF size; native nm as fallback), `width_or_type`, `access_widths`,
`mutability`, `semantic_family`, `occurrences`, `section`. There are **no
anonymous raw-address bindings** — every binding resolves by symbol.

| Family | Bindings | Unique symbols | Occurrences | Mutability |
|---|---:|---:|---:|---|
| ENGINE_EWRAM_TARGET | 50 | 23 | 488 | 50 mutable |
| ENGINE_CALLBACK | 280 | 280 | 1,030 | 280 mutable (.text) |
| ENGINE_TABLE_TARGET | 50 | 50 | 89 | 50 const (.rodata) |
| ENGINE_SPRITE_TEMPLATE_TARGET | 327 | 327 | 2,108 | 327 const (.rodata) |
| ENGINE_GFX_TARGET | 11 | 11 | 36 | 11 const (.rodata) |
| **Total** | **718** | | **7,549** | 330 mutable / 388 const |

All 718 are u32-typed slots. The EWRAM set (23 symbols: gBattleScripting
(with sB_ANIM_ARG1 etc.), gBattleCommunication, gHitMarker,
gMoveResultFlags, gBattleTypeFlags, gBattleMoveDamage, gBattlerTarget,
gCurrentMove, gBattlersCount, gBattlerAttacker, …) is the complete battle
engine-data surface reachable from bytecode operands.

## 5. Nonzero addends (brief §5 — STOP gate)

3,058 relocation sites carry a nonzero addend; every one is classified and
bounds-validated (`all_bounds_ok = true`):

| Kind | Count | Meaning |
|---|---:|---|
| section-relative-target | 2,778 | anim/AI-style relocs against the section symbol; the addend **is** the root label's section offset (the interior-target count is 0, so no addend points into the middle of a span) |
| byte-field | 264 | u8-width EWRAM accesses (gBattleCommunication+5/6, gBattleMoveDamage+1..3, setbyte/addbyte/… targets) |
| struct-member | 13 | u32-width EWRAM accesses (playanimation's sB_ANIM_ARG1 = gBattleScripting+0x10, …) |
| halfword-field | 3 | u16-width accesses (sethword/copyhword/jumpifhalfword targets) |

array-element/table-entry = 0; unexplained = 0. The 280 ENGINE addends
reduce to 27 unique (symbol, addend) pairs, all EWRAM — no callback,
template, gfx, or compiled table is ever accessed at a nonzero offset.
Bounds rule: `addend + max(access_widths) <= allowed_offset` for every
binding; no violation. NULL-literal census: 65 stored-zero u32 pointer slots
with no relocation (tryfaintmon 31, playanimation 30, playanimation_var 2,
jumpifword 1 scalar, bs2 1) — intentional NULL operands, not unresolved
relocations.

## 6. Engine-table census (brief §6)

13 routing tables inside the H objects, 768 rows (764 pointer + 4 value):

| Table | Rows | Width |
|---|---:|---:|
| gBattleScriptsForMoveEffects | 214 | u32 ptr |
| gBattleAnims_Moves | 356 | u32 ptr |
| gBattleAnims_StatusConditions | 9 | u32 ptr |
| gBattleAnims_General | 23 | u32 ptr |
| gBattleAnims_Special | 7 | u32 ptr |
| gFieldEffectScriptPointers | 67 | u32 ptr |
| gBattleAI_ScriptsTable | 32 | u32 ptr |
| gContestAI_ScriptsTable | 32 | u32 ptr |
| gBattlescriptsForBallThrow | 13 | u32 ptr |
| gBattlescriptsForUsingItem | 6 | u32 ptr |
| gBattlescriptsForRunningByItem | 1 | u32 ptr |
| gBattlescriptsForSafariActions | 4 | u32 ptr |
| gMovesWithQuietBGM | 4 | **u16 value** (move IDs) |

The compiled u16 string-ID tables (`gStatUpStringIds` 20, `gStatDownStringIds`
10, etc.) are engine-owned `.rodata` — 81 battle occurrences (74+7) + 8 anim
gIceCrystal* occurrences, bound through the §4 table (50 unique table
symbols), not owned by H bytecode.

## 7. Battle module ownership (brief §7)

Per-span modules, each byte owned exactly once, keyed
`emerald:battle-script/<dash-normalised label>`: **2,089 modules / 2,089
unique keys**, per-object contiguous `[start, end)`, total 90,047 B — the
sum of the six qualified object sizes, no overlap, no gap inside any object.
Empty alignment spans (8 total: bs1 5, anim 3) are carried as zero-byte
modules for label identity.

## 8. Interior target proof (brief §8)

| Object | Script operands | Root targets | Interior targets | Distinct roots |
|---|---:|---:|---:|---:|
| battle_scripts_1 | 962 | 962 | **0** | 439 |
| battle_scripts_2 | 31 | 31 | **0** | 18 |
| battle_anim_scripts | 1,152 | 1,152 | **0** | 648 |
| battle_ai_scripts | 1,222 | 1,222 | **0** | 550 |
| contest_ai_scripts | 364 | 364 | **0** | 158 |
| field_effect_scripts | 67 | 67 | **0** | 67 |
| **Total** | 3,798 | 3,798 | **0** | **1,880** |

Every script-target relocation resolves to exactly one label (root). Top
fan-in matches the plan's §5 prediction exactly: BattleScript_MoveEnd 137,
BattleScript_ButItFailed 95; AI's hottest root is Score_Minus10 (110).

## 9–10. Animation grammar and closure (brief §9, §10)

48-opcode anim grammar (51 encodings, zero size aliases), 63,811 B, 658
spans, 9,215 instructions, 4,231 relocs — all classified: 2,108 sprite
templates (327 distinct), 963 callbacks (213 distinct), 1,152 in-family
targets, 8 gIceCrystal* table targets, 0 unresolved/ambiguous. All 5 anim
routing tables (including the u16 gMovesWithQuietBGM) counted in §6.

## 11. Field-effect independent audit (brief §11)

817 B, 68 spans (67 scripts + gFieldEffectScriptPointers), 137
instructions, 170 relocs: 67 script targets (all 67 gFieldEffectScript_*
roots, each fan-in 1), 67 callnative callbacks, 36 gfx targets (11 distinct
gSpritePalette_*/gfx symbols). Split 36/67 vs plan ≈40/≈63 — total 103
exact; the gfx subset was overestimated and the callnative subset
underestimated.

## 12–13. Battle AI and contest AI grammar + graph (brief §12, §13)

- Battle AI: 99-opcode grammar (114 encodings), 9,303 B, 553 spans (527
  script + 25 typed data + 1 routing), 1,738 instructions, 1,222 relocs —
  **1,222/1,222 in-family SCRIPT_TARGET**; zero engine-data or table
  targets, zero unresolved. Data spans are typed tables (trainer-mon
  structures etc.) owning 0 relocs; the 32 gBattleAI_ScriptsTable rows are
  routing.
- Contest AI: 136-opcode grammar (140 encodings), 2,524 B, 165 spans (164
  script + 1 routing), 613 instructions, 364 relocs — **364/364 in-family**.
  The qualified macro file's `.4bye` typo (opcode 0x30, never invoked in
  the qualified data) is recorded in the grammar with the intended width.

## 14. Cross-family pointer oracle (brief §14 — STOP gate)

Every one of the 7,549 records' final target was tested against all six H
arena ranges. **Cross-family bytecode pointer edges: 0.** battle→anim 0,
battle→AI 0, battle→FE 0, AI→battle 0, anim→battle 0, FE→battle 0. The
battle arena hull (14,413 B) nests the FE arena (817 B) as in G; no record
of one family resolves inside another family's arena. `playanimation`'s
anim ID is an EWRAM arg slot (`sB_ANIM_ARG1`), never an anim label — the
R13-A "battle→gBattleAnims_Moves 365" figure describes the engine-side ID
routing table, not a bytecode edge. The three live-cutover waves (H4/H5/H6)
are therefore provably independent.

## 15. R13-C text prerequisite (brief §15)

**0** text-ID operands in any family. Battle text is 2-byte STRINGID →
C-owned `gBattleStringsTable`; the only text-adjacent operands are the
compiled u16 ID tables (ENGINE_TABLE_TARGET, §6). The R13-C
prerequisite is satisfied vacuously.

## 16–17. State-v5 surfaces, save legality, capture policy (brief §16, §17)

9 EWRAM surfaces, 40 slots total (all quiescent-owned, save-legal):

| Surface | Slots | Stable form |
|---|---:|---|
| gBattlescriptCurrInstr | 1 | EWRAM — H key + instruction offset (INSTRUCTION_START) |
| battleScriptsStack.ptr[] | 8 | GAME_BSS — H key + next-instruction offset (IP+5 returns) |
| battleCallbackStack.function[] | 8 | GAME_BSS — engine function image (unchanged) |
| gSelectionBattleScripts[]/gPalaceSelectionBattleScripts[] | 8 | EWRAM — H key + offset |
| gAIScriptPtr | 1 | EWRAM — H key + offset (quiescent stale IP) |
| AI_ScriptsStack.ptr[] | 8 | GAME_BSS — H key + offset (size 0 at VBlank) |
| sBattleAnimScriptPtr/sBattleAnimScriptRetAddr | 2 | EWRAM — H key + offset |
| gAnimScriptCallback | 1 | EWRAM — engine function (existing special-case) |
| sTrainerBattleEndScript + A/B returns | 3 | EWRAM — existing G slots, class widened to H targets |

Worst case ≈19 quiescent / ≈27 mid-battle records, sidecar cap 4,096 —
headroom ≈150×. Capture policy: the sidecar (key, rangeOffset) resolves
through the family arena; interior IPs/stack offsets are legal because the
arena is one registered range (§18).

## 18. Range-cap model (brief §18 — hard gate)

Five family arenas (one per family; battle hull nests FE), registered as
**5 ranges, not per-root** (per-root ≈2,060 ranges would exceed the 8,192
cap at 8,437):

| Arena | Hull | Bytes |
|---|---:|---:|
| battle (nests FE) | 0x082d86a8–0x082dbef5 | 14,413 |
| battle_anim | 0x082c8d64–0x082d86a8 | 63,811 |
| battle_ai | 0x082dbef8–0x082de350 | 9,303 |
| contest_ai | 0x082de350–0x082ded24 | 2,524 |
| field_effect | 0x082db9d4–0x082dbd05 | 817 |

Post-H live ranges: 6,377 + 5 = **6,382** of 8,192 (headroom 1,810);
pack entries 20,988 + 2,089 modules (incl. 13 routing spans + 8 empty
alignment spans) = **23,077** of 32,768 (the plan's ≈23,049 estimated on
≈2,056 modules; corrected to the exact module count); canonical bytes
**90,047**; sidecar ≈27 worst-case of 4,096. Module+offset identity is
preserved via the sidecar + internal module reverse-containment index,
exactly per plan §18. The range cap is **not** increased; the format is
unchanged.

**H2 reconciliation (2026-08-22):** the H2 pack build reconciled the
23,077 arithmetic to **23,069** — the 8 zero-width alias modules (4
battle-script effects + 4 battle-anim moves) share their base module's
address and carry no payload, so they are catalog/ownership identities
only and cannot be pack records. Pack = 20,988 + 2,081 payload records.
See `docs/R13H2_BATTLE_SCRIPT_RESOURCE_REPORT.md` §8.

## 19–20. H2 resource identities and artifact schema (brief §19, §20)

Identities: `emerald:battle-script/<root>` (2,089 modules), 13 routing
tables under their table identities, plus the §4 engine-binding table
(718 rows). H2 generated-artifact schema is the 10-file family below; the
generator's `--check` re-emits every file into a temp dir and byte-compares
— **second run byte-identical (10/10 files)**:

`h1_family_inventory`, `h1_relocation_census`, `h1_engine_bindings`,
`h1_routing_rows`, `h1_interior_target_proof`, `h1_modules`,
`h1_census_closure`, `h1_oracle`, `h1_state_v5_surfaces`,
`h1_addend_audit` (all `.generated.toml`), + `h1_grammar_*.generated.toml`
(5) + `_h1_debug.json`.

## 21. Static oracle (brief §21)

For all 7,549 records: ROM u32 == ELF u32 == generated final_gba.
**7,549 checked, 0 bad.** (The ELF oracle reads the qualified linked ELF's
PROGBITS sections at the same GBA addresses; the ROM oracle reads the raw
qualified ROM.)

## 23. Grammar fixtures (brief §23)

`h1_grammar_battle|battle_anim|battle_ai|contest_ai|field_effect
.generated.toml` — 5 fixtures, counts per §2, cross-checked against the
qualified C tables. Ready for H2's stepwise-differential oracle harness.

## 24–25. G infra reuse and H stage split (brief §24, §25)

Confirmed: **H1–H7 split exactly as planned** (§17 of the plan): H1 analysis
(this stage), H2 additive module resources, H3 State-v5 execution readiness
(force-different-base dry-run harness; STOP gate = nested interior-IP
restore), H4 anim+FE live cutover, H5 battle VM live cutover, H6 AI+contest
live cutover, H7 compiled removal/isolation. Infra reuse confirmed as plan
§19: **B** (extract a bounded generic generator core parameterized per VM
from `tools/gen3_resources/script_family/`, with G's artifacts regenerating
byte-identically) before H; **C** (parallel sibling seams, no live-seam
refactor) for the runtime; **A** (pack format, schemas, sidecar, three-phase
arena machinery) reused as-is.

## 26. Deliverables

This report + the §23 fixtures + the 10 analysis files above; plan updated
with the confirmed facts (see `R13H_BATTLE_SCRIPT_MIGRATION_PLAN.md`
§"Confirmed by R13-H1").

## 27. STOP conditions

Respected: analysis/generation/oracle only; no production code touched
(working tree delta vs R13-G5 is only untracked H1 artifacts); no commit;
no H2 start; no R13-I start.

## Final pins (brief closing requirement)

- **Battle pins:** 13,592 B (13,099+493; −4 vs plan, padding), 1,562 relocs
  exact, 645 spans, 993 SCRIPT / 488 EWRAM / 81 TABLE targets.
- **Animation pins:** 63,811 B (−1), 4,231 relocs exact, 658 spans; 2,108
  templates / 963 callbacks / 1,152 script / 8 table.
- **Field-effect pins:** 817 B (−3), 170 relocs exact, 68 spans; 67 script /
  67 callback / 36 gfx.
- **Battle-AI pins:** 9,303 B exact, 1,222 relocs exact, 553 spans;
  1,222/1,222 in-family.
- **Contest-AI pins:** 2,524 B exact, 364 relocs exact, 165 spans;
  364/364 in-family.
- **Engine-data binding census:** 718 bindings (488 EWRAM / 1,030 callback /
  89 table / 2,108 template / 36 gfx occurrences), all symbol-resolved, no
  anonymous raw addresses.
- **Engine-table census:** 13 routing tables / 768 rows (764 ptr + 4 u16);
  81 battle string-ID table occurrences exact.
- **Nonzero addends:** 3,058 = 2,778 section-relative targets + 280 EWRAM
  fields (264 byte + 13 u32 + 3 halfword; 27 unique pairs); bounds all OK;
  unexplained 0. NULL sentinels 65.
- **Root/interior:** 3,798 script operands, all root, **interior 0**,
  1,880 distinct targets; MoveEnd 137 / ButItFailed 95 (plan-exact).
- **Cross-family edges:** 0.
- **State-v5 / family ranges:** 9 surfaces / 40 slots; 5 arena ranges →
  6,382/8,192; pack 23,069/32,768 (H2 reconciliation: 23,077 minus 8
  zero-width alias modules — see §18 note); canonical bytes 90,047; sidecar
  ≈27/4,096.
- **H2–H7 split:** confirmed (H2 additive resources → H7 isolation);
  blockers: none.
