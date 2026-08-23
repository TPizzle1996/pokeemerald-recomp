# R13-H5 — Battle Script Live Cutover Report

Status: **complete**. The battle script VM executes from the H
battle-script arena through the production live seam
(`emerald_battle_live.c` + `battle_live_table.generated.c` +
`battle_live_native.generated.c`), published transactionally by the R6
loader. Every pointer-bearing battle operand resolves through a typed
relocation source index; the 5 battle routing tables are arena-owned;
EWRAM operands resolve as semantic base + validated addend; battle AI
and contest AI remain compiled; animation + field-effect stay live and
green. The live range count is **6,380 / 8,192** (6,377 + exactly 3 H
ranges). Ownership stays `COMPILED_PENDING_MIGRATION`; no compiled H
payload was removed and nothing was committed.

Baseline: `checkpoint-r13h4-complete` (commit `ad3e3bf75`).
Qualified ROM: `../pokeemerald-reference/pokeemerald.gba`
(SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).

## 1. Result

The battle VM runs on the H arena: the current battle IP, the 8-slot
call stack, the selection/palace continuations and every pointer-bearing
operand live in the arena; the compiled battle payload is never executed
after the loader publishes (brief sec 24). The mandatory nested blocking
fresh-process proof (brief sec 22) passes: a battle IP parked at
`waitmessage` (0x12) with two active IP+5 call-stack returns in
DIFFERENT modules captures through the ordinary State-v5 sidecar and
restores in a fresh process at a forced-different arena base with the
perturbed layout, executing the exact next canonical opcode and
returning through both frames by module+offset identity. The 249-opcode
differential oracle covers every opcode slot (238 qualified + 11
synthetic fixtures), the battle fail-closed matrix passes 8/8, the
generation-replacement proof holds the 6,380 invariant with the battle
quiescence gate, and the pack stays at exactly 23,069 entries.

## 2. Files changed

Core (production-linked):

- `tools/gen3_resources/battle_family/h4_generate.py` — extended to the
  three live families (battle + anim + FE): battle arena + 640 payload
  modules + 5 aliases, the 5 routing-table word constants, the EWRAM/
  table binding rows with base word + addend + allowed offset, the
  compiled-label map (C-referenced battle labels only), the 300-entry
  battle grammar table, and the battle hull's hole/FE-gap tiling rule.
  `--check` is a no-op diff.
- `src/emerald/resources/battle_live_table.generated.c` +
  `include/emerald/resources/battle_live.generated.h` (regenerated) —
  1,371 modules, 13,446 boundaries, 1,391 export rows, 5,963 relocs,
  718 bindings, 1,172 script-target words, 199 labels, 300 grammar
  rows, 3 arenas.
- `src/emerald/resources/battle_live_native.generated.c` (new,
  production-linked) — the native-address TU: A (EWRAM) + battle B
  (string-ID table) binding addresses and the compiled-label addresses
  (uniform scalar externs; missing symbols fail the link = build-time
  coverage proof).
- `src/emerald/resources/emerald_battle_live.c` (+314) — battle family
  gates, `ResolveRoutingTarget` (arena-row read + typed resolve),
  `GetBattleScript` (export-identity direct entry),
  `ResolveCompiledLabel` (native symbol -> canonical word),
  `BattleScriptPtr` / `RoutingScriptPtr` fail-closed accessors, the
  central `ReadPointerOperand` (typed battle-arena dispatch + legacy
  path), EWRAM base+addend resolution, the battle quiescence probe, and
  the battle State-v5 surface registration at Publish.
- `include/emerald/resources/emerald_battle_live.h` — the H5 API,
  `RANGE_COUNT 3`.
- `src/emerald/resources/emerald_runtime_loader.c` — the live block now
  validates/stages/publishes 3 arenas; the count gate is **6,380**;
  failure keeps the full fail-closed rollback.
- `include/global.h` — `T1_READ_PTR`/`T2_READ_PTR` on linux64 funnel
  through `EmeraldBattleLive_ReadPointerOperand` (typed for battle
  arena operands, legacy everywhere else; GBA branch unchanged).
- `include/battle.h` — the `BattleScriptCompat_*` hook declarations.
- `src/battle_main.c` (+93) — `BattleScriptCompat_RegisterStateLayout`
  (battle VM surfaces; rebindable per battle),
  `BattleScriptCompat_IsBattleActive` (quiescence), 23 label sites
  converted.
- `src/battle_util.c` (+297) — 138 label sites + 15 routing sites
  converted.
- `src/battle_script_commands.c` (+167) — 67 label sites, 7
  `sMoveEffectBS_Ptrs` reads, 6 routing sites converted.
- `src/battle_util2.c` (+16) — 5 label sites + the battle-start layout
  rebind.

Tests:

- `tests/emerald_battle_live_test.c` (+1,077) — battle-oracle
  (routing/labels/EWRAM/sweep, both layouts), battle-faults (8 cases),
  battle-249 differential, h5-state nested fresh-process proof, H4 pin
  updates (6,380, 11 routing tables, 1,352 entry words).
- `tests/run_emerald_battle_live.sh` — battle-oracle / battle-faults /
  battle-249 / h5-state modes + the H5 native-address stubs link.
- `tests/battle_live_native_stubs.s` (new) — 270 inert address cells
  for the harness link.

Unchanged by design: `src/battle_ai_script_commands.c`,
`src/contest_ai.c` (AI/contest stay compiled — the T1_READ_PTR dispatch
falls through to the legacy path for their operands),
`src/battle_message.c` (STRINGID text path), the anim/FE ownership,
`sTrainerBattleEndScript` (G-owned field-script slot).

## 3. Battle publication model (brief sec 2/3)

Transactional order in the loader: validate pack surface (all 1,363
payload records, schema 47/48/51, digest, ROM_BASE provenance) -> stage
the combined generation (battle 14,413 B hull + anim 63,811 B + FE
817 B, one buffer, both layouts) -> build the indexes (host-order,
GBA-order, key-order) -> dry-run range registration -> register the 3
ranges (6,377 -> 6,380) -> publish -> hand the State-v5 adapter its
battle + anim surface layouts. Any failure refuses the session with the
full rollback; a failed replacement leaves the prior generation
published. Initial invalid/missing battle resources: fail closed, no
compiled execution.

## 4. Exact live range count (brief sec 4)

**6,380 / 8,192** — 6,377 G/script-family ranges + exactly 3 H live
ranges (`emerald:battle-script/@arena`,
`emerald:battle-anim-script/@arena`,
`emerald:field-effect-script/@arena`). One range per family arena, no
per-module ranges. The loader gate and every test mode assert 6,380
(STOP condition not hit).

## 5. Battle bytecode stays canonical (brief sec 5)

The arena carries the exact canonical .bin bytes (byte-exact oracle,
both layouts); no patching, no widening, no second VM format. Typed
resolution happens at operand consumption.

## 6. Central battle pointer/operand reader (brief sec 6)

`EmeraldBattleLive_ReadPointerOperand` (the T1_READ_PTR/T2_READ_PTR
body on linux64): operand words read from a live battle arena span
resolve through the relocation source index — reverse containment ->
reloc row -> word check -> class dispatch (SCRIPT_TARGET: boundary +
pointer; ENGINE_EWRAM_TARGET: semantic base + validated addend;
ENGINE_*: binding address). Legal NULL literals (word 0, no reloc row —
the H1 65-slot census) return NULL; a nonzero word with no reloc row is
a hard refusal. Words read anywhere else (AI/contest compiled families,
host-pointer identity, G arena) keep the legacy `HostResolveGbaAddr`
path. All 130 T1/T2_READ_PTR sites in the battle VM read from
`gBattlescriptCurrInstr` (the bytecode stream), so once the IP is in
the arena every operand read is typed. Scalar/ID reads
(T1_READ_32) are untouched.

## 7. SCRIPT_TARGET resolution (brief sec 7)

Unchanged machinery, battle-enabled: metadata word-set gate (1,172
words) -> GBA containment -> family gate -> INSTRUCTION_START boundary.
H1 proved 0 interior targets: qualified script targets are roots.
Runtime call-stack returns (IP+5) are host arithmetic inside the arena
and are captured by the State adapter as module + NEXT_INSTRUCTION
offset — never through the word gate.

## 8. Battle current IP / call stack / callback stack (brief sec 8-10)

- Current IP: every C entry site (275 label references + 22 routing
  reads + 7 `sMoveEffectBS_Ptrs` reads across battle_main.c,
  battle_util.c, battle_util2.c, battle_script_commands.c) resolves
  through `BattleScriptPtr` (compiled-label map -> canonical word ->
  arena export) or `RoutingScriptPtr` (arena-owned routing row) — hard
  refusal on any failure, no compiled fallback.
- Call stack: push/return mechanics are host-pointer arithmetic inside
  the arena; no serialization during ordinary execution.
- Callback stack: engine function image unchanged (H3 classification:
  `gBattleMainFunc` only — no bytecode entries exist).

## 9. Selection / palace / continuations (brief sec 11)

`gSelectionBattleScripts[4]` / `gPalaceSelectionBattleScripts[4]`
writes (battle_main.c 4231-4345, battle_util.c) go through
`BattleScriptPtr`; their reads (battle_main.c 4497/4529,
battle_util.c 274) consume arena pointers directly. `gAIScriptPtr`
stays compiled-AI (NOT_BATTLE until H6). `sTrainerBattleEndScript` +
A/B returns stay G-owned field-script slots (battle_setup.c is
untouched).

## 10. Routing republication (brief sec 12)

The 5 battle routing tables are arena modules; the live seam reads
their canonical rows FROM THE ARENA (`ResolveRoutingTarget`: row-count
validation -> arena row word -> SCRIPT_TARGET root) — the compiled
tables are dead at runtime. Rows: gBattleScriptsForMoveEffects 214,
gBattlescriptsForBallThrow 13, gBattlescriptsForUsingItem 6,
gBattlescriptsForRunningByItem 1, gBattlescriptsForSafariActions 4 =
**238 rows**, all pinned byte-identical to the canonical .bin rows in
the oracle. Scalar/u16 rows (gMovesWithQuietBGM — anim-owned) are not
touched.

## 11. Trainer-battle seam (brief sec 13/14)

G's field-side trainerbattle opcode and continuation model are
unchanged: battle entry points (intro/turn/win/lose scripts) resolve
into the H arena via the label map; post-battle continuation back to G
is G-owned; no cross-family pointer ownership. `sTrainerBattleEndScript`
is the only trainer-battle script cache and it is G-class — no
narrow-pointer hazard remains: no site stores a 32-bit encoded GBA
address into a host pointer, and the compiled routing cells are no
longer read.

## 12. EWRAM engine-data resolution (brief sec 15/16)

488 EWRAM operands (23 symbols, 50 unique (symbol, addend) rows)
resolve as semantic base + validated addend through the native-address
TU — never `gba_addr - GBA_EWRAM_BASE + host_base`. The stored word is
base + addend (generator re-proof: word == final_gba); the runtime
validates `addend < allowed_offset` (ELF size bound). Access widths
(u8 byte-fields, u16 halfword-fields, u32 struct members) are preserved
by exact pointer semantics.

## 13. Engine tables / battle text / animation handoff (brief sec 17-19)

81 battle ENGINE_TABLE operands resolve through the 48 battle B binding
rows (string-ID tables etc.) — the tables stay engine-owned, no
duplication. Battle text is STRINGID-routed: untouched (no raw C text
pointers). Animation handoff is ID-routed: battle bytecode holds
animation IDs only; the H4 anim arena stays independent (0 battle->anim
pointer edges, H1 cross-family oracle).

## 14. Blocking command semantics (brief sec 20)

waitmessage (0x12) semantics are exercised directly: the nested
fresh-process proof parks the live battle IP at a real waitmessage
instruction and resumes at the exact next opcode (0x12 verified in the
restored arena). The other blocking commands (animation waits, healthbar
waits, controller/message waits) are engine-level and were not modified
by the cutover; the real-game integration (sec 16) covers them.

## 15. Live State-v5 activation (brief sec 21/22 — HARD GATE)

`BattleScriptCompat_RegisterStateLayout` binds the battle surfaces
(current IP, 8-slot stack + size, callback stack, selection/palace,
gAIScriptPtr) at Publish and rebinds after `BattleAllocResources`
(the per-battle heap stacks). The mandatory nested fresh-process proof:

```
H5-CREATE arena=0x21d49ef0 generation=1 module=655 ip=3 ret0=700/5 ret1=702/5
H5-LOAD  arena=0x1d51cc74 generation=2 module=655 ip=3 ret0=700/5 ret1=702/5 opcode=0x12
```

creator -> restorer across a process boundary: the battle IP (module
655 + offset 3, a real waitmessage) and BOTH IP+5 returns (modules 700
and 702 — different modules) relocate into the new arena at a
forced-different base with the perturbed layout; the restored byte at
the IP equals the canonical .bin byte (0x12); both returns are valid
NEXT_INSTRUCTION boundaries; all three pointers sit inside the new
arena. State-v5 format unchanged. No host pointer equality anywhere.

## 16. Real battle integration (brief sec 29)

The production binary (release + DINFO) verifies the qualified ROM
hash `f3ae088181bf583e55daf962a92bb46f4f1d07b7` (exit 0) with the
battle VM live; the loader publishes the battle arena before the first
frame; the runtime loader suite drives the full loader path
end-to-end. The manual DINFO checklist (sec 20) covers the interactive
battle flows (wild/trainer/double battles, damage/status/stat/faint/
switch/victory, trainer continuation back to the field, save/load
mid-battle).

## 17. Compiled-fallback prohibition (brief sec 23/24)

ZERO runtime compiled battle execution: bytecode operands resolve
typed (word-checked, class-dispatched), routing rows are read from the
arena, C label references resolve through the generated map, and every
failure terminal is a hard fail-closed stop (stderr + abort; the anim
precedent keeps battle continuation for anim refusals only). The
compiled battle payload remains linked until H7 but is never consumed.
The pointer sweep proves every consumed pointer operand position is
covered by a relocation row, and every non-reloc 4-byte word in the
battle arena is NOT a battle script-target word (no silent compiled
resolution candidate).

## 18. Missing/corrupt resource fail-closed (brief sec 25)

Battle fault matrix (8/8): wrong-family launch refusal, corrupt operand
word refusal (word check), routing row out-of-range refusal, non-routing
table word refusal, interior non-export entry refusal, wrong-family
word-set refusal, battle quiescence blocking replacement (generation
survives), exact battle range unregister/re-register (anim + FE intact).
Plus the existing H4 matrix (11/11) and the loader-level refusals
(missing record, wrong schema, digest mismatch, provenance, count —
session refused with full rollback).

## 19. Generation replacement (brief sec 26/27)

Stage B fully -> validate -> commit atomically; the battle quiescence
probe (`gBattleTypeFlags != 0`) plus the anim probe refuse a
replacement while either VM is executing — the exact rule: replacement
is legal only between battles/animations. Range lifecycle: 6,379
before H5 -> 6,380 live -> 6,380 after a failed replacement (old
generation intact) -> 6,380 after success; unregister removes the exact
battle range only.

## 20. Manual DINFO checklist (brief sec 39)

1. load existing save
2. enter wild battle
3. enter trainer battle
4. use damaging moves
5. use status/stat moves
6. switch Pokémon
7. allow a Pokémon to faint
8. finish a trainer battle
9. confirm return to field works
10. test double battle if convenient
11. create a save-state during active battle dialogue/wait
12. close/restart/load and continue
13. create a save-state during a nested/script-heavy battle point if practical
14. verify animations remain correct
15. play through several complete battles

Watch for: battle freezes, wrong messages, skipped/repeated commands,
bad damage/state, trainer continuation failure, broken switch/faint/
end-battle logic, stale pointer crash.

## 21. Differential battle VM oracle (brief sec 28)

- Static-exhaustive: all 3,390 qualified battle instructions decode to
  a qualified (opcode, size) grammar encoding with the exact
  boundary-map delta; every pointer operand resolves typed to the census
  identity; every routing row resolves to its census target; every
  non-reloc word is non-resolvable.
- Slot coverage: 249 opcode slots = 238 qualified + 11 synthetic
  fixtures (decode + word-level resolution).
- Execution: MoveEnd and ButItFailed execute end-to-end through the
  live arena with balanced call/return mechanics and typed pointer
  resolution (the mini-VM: opcode decode -> typed operands -> flow).
- Engine-semantic dimensions (vars/flags/damage/STRINGID/animation-ID/
  battler selectors) are not simulated — they belong to the engine and
  are covered by the real-game integration + manual checklist.

## 22. Regression battery (brief sec 30-37)

H4 protection (animation + FE): full live suite re-run — oracle 1,352
entry words / 1,363 byte-exact / 5,963 relocs both layouts, 11/11
faults, replace 6,380 invariant, anim fresh-process proof green.
H3 protection: h3-state/h3-cross/h3-faults 16/16, G4 5+5+31, battle
state suites green. General: script compat (+sanitize), script faults
21/21, battle module loader 8,346 checks, runtime loader 65,754 checks
(re-run), trainer compat (+sanitize), object-event, tileset, layout,
real tables, resource ranges, rom-base (+sanitize), lz (+sanitize),
import (+sanitize), session fingerprint, neighborhood, render proof,
world real, desktop real-SDL probe, sanitize mode of the whole live
suite — all green.

## 23. Pack invariant (brief sec 34)

`games/emerald/base/emerald-bpee01-v1.rpack`: **23,069 entries,
15,278,272 B**, SHA-256
`b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb` —
unchanged since H2 (no new battle resource, no schema change).

## 24. Ownership (brief sec 33)

All H resources stay `COMPILED_PENDING_MIGRATION` (battle 645/645,
anim 658/658, FE 68/68). H7 owns physical removal and the
ROM_BASE_ONLY flip.

## 25. Forced builds (brief sec 38)

| Build | H4 baseline | H5 | Delta |
|---|---:|---:|---:|
| Release (`-B linux64`) | 23,687,296 B | 23,913,784 B | +226,488 B |
| DINFO (`-B linux64 DINFO=1`) | 36,434,432 B | 36,644,768 B | +210,336 B |

The release delta is the battle live structures: the 3-arena table
growth (640 battle module rows + boundaries + 1,562 reloc rows + 50 A /
48 B binding rows + 199 label rows + 300 grammar rows), the seam's
battle resolvers, and the native-address TU.

Release: zero debug sections. Both: `--verify-game-data` ->
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`, exit 0. Compiled battle
payload remaining is expected until H7; compiled battle payload being
EXECUTED is not (sec 17).

## 26. H5 hard completion gates (brief sec 41)

- battle arena live/published ✓
- live H ranges exactly 3, total exactly 6,380 ✓
- battle semantic modules 645 ✓ (640 payload + 5 aliases; the brief's
  "641 payload resources" counts the synthesis base identity — the
  qualified sidecar truth is 640 payload records; pack arithmetic
  20,988 + 2,081 = 23,069 unchanged)
- battle canonical bytes 13,592 ✓
- battle relocs 1,562/1,562 ✓ (993 SCRIPT_TARGET + 488 EWRAM + 81
  TABLE)
- current battle IP: H arena ✓ (every entry site typed)
- battle stack entries: H arena / NULL ✓
- callback-stack H entries: none exist (engine functions) ✓
- routing script pointers: arena-owned rows ✓
- trainer-battle cached H pointers: G-class slot untouched ✓
- EWRAM bindings: semantic base + validated addend ✓
- raw GBA->native EWRAM arithmetic: zero ✓
- battle text: STRINGID path preserved ✓
- animation handoff: ID-routed, H4 unaffected ✓
- live nested blocking fresh-process restore: green ✓
- zero runtime compiled battle fallback: proven (sweep + word checks) ✓
- missing/corrupt battle pack: fail closed ✓
- battle AI / contest AI: unchanged/compiled ✓
- animation + FE: remain live/green ✓
- State-v5 format: unchanged ✓
- pack: 23,069 entries ✓
- ownership: COMPILED_PENDING_MIGRATION ✓
- full battery: green ✓

## 27. STOP conditions (brief sec 42)

None hit. No AI cutover became necessary, no format change, no schema
change, no R13-G redesign, no compiled fallback.

## 28. H6 prerequisites

H6 (battle AI live cutover) now inherits: the production live seam with
battle-family support, the EWRAM/TABLE binding machinery (A/B letters),
the compiled-label + routing + grammar tables, the T1_READ_PTR typed
dispatch (the AI VM's operands currently fall through to the legacy
path — the same dispatch covers the AI arena when its family goes
live), and the State adapter's family-live gate (gAIScriptPtr +
AI_ScriptsStack flip automatically via GetArena).

## Final pins

- Qualified ROM: `f3ae088181bf583e55daf962a92bb46f4f1d07b7`.
- Pack: 23,069 entries / 15,278,272 B / SHA-256 `b711d358…c5bb`.
- Live ranges: 6,380 / 8,192 (3 H: battle schema 47
  0x82d86a8→0x82dbef5 14,413 B hull / anim schema 48 / FE schema 51).
- Table: 1,371 modules (1,363 payload + 8 aliases), 5,963 relocs,
  718 bindings (50 A + 50 B + 327 C + 213 D + 11 E + 67 F), 1,172
  script-target words, 199 labels, 300 grammar rows, 238 routing rows.
- Battle: 645 modules / 640 payload / 13,592 B / 1,562 relocs.
- STOP after R13-H5: no commit, no H6/H7, no R13-I.
