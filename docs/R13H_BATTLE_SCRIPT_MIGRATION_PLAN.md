# R13-H — Emerald Battle/Animation/AI Script Ownership Migration Plan

Status: **architecture/inventory/implementation-plan only; implementation has
not started**

Baseline: `checkpoint-r13g-complete` (commit `1f3aa089b`)

Qualified ROM: `../pokeemerald-reference/pokeemerald.gba`

Qualified ELF: `../pokeemerald-reference/pokeemerald.elf`

Qualified ROM SHA-1: `f3ae088181bf583e55daf962a92bb46f4f1d07b7`

R13-H owns the remaining battle-related script bytecode families: battle
scripts, battle animation scripts, battle AI scripts, contest AI scripts, and
field-effect scripts. It reuses the R13-G provenance model (exact ROM bytes,
typed relocation sidecars, no operand patching, no widening) where each VM's
grammar and execution semantics permit — and it does **not** assume G's
geometry blindly: the decisive audit finding is that every cross-family
reference in these VMs is **ID-routed through engine tables**, not
pointer-routed, and that a large operand class targets **engine-owned**
EWRAM data, sprite templates, and C functions rather than resources.

## 1. Exact H inventory

Recomputed from the current R13-G-complete tree and the qualified GBA ELF
object files (`../pokeemerald-reference/build/emerald/data/*.o` — relocation
tables intact), cross-checked against the qualified ROM symbol table, the
`script_data` section order in `../pokeemerald-reference/ld_script.txt`, the
source macro files, and the repo's own linux64 objects.

### 1.1 Per-family census (qualified GBA build)

| Family | Data file | Labels | Object span | Canonical bytes | 4-byte relocs |
|---|---:|---:|---:|---:|---:|
| Battle scripts | `data/battle_scripts_1.s` | 617 `BattleScript_*` + 26 local + 1 | battle_scripts_1.o 0x082d86a8–0x082db9d4 | **13,100** | 1,508 |
| Battle item-routing scripts | `data/battle_scripts_2.s` | 22 `BattleScript_*` + 4 local | battle_scripts_2.o 0x082dbd08–0x082dbef8 | **496** | 54 |
| Battle anim scripts | `data/battle_anim_scripts.s` | 356 `Move_*`, 23 `General_*`, 7 `Special_*`, 14 `Status_*`, 5 `Frustration_*`, ≈250 bare effect labels, 5 tables (≈657 script labels) | battle_anim_scripts.o 0x082c8d64–0x082d86a8 | **63,812** | 4,231 |
| Battle AI scripts | `data/battle_ai_scripts.s` | 533 `AI_*` + 22 other labels | battle_ai_scripts.o 0x082dbef8–0x082de350 | **9,303** | 1,222 |
| Contest AI scripts | `data/contest_ai_scripts.s` | 164 `AI_*` + 1 | contest_ai_scripts.o 0x082de350–0x082ded2c | **2,524** | 364 |
| Field-effect scripts | `data/field_effect_scripts.s` | 67 `gFieldEffectScript_*` | field_effect_scripts.o 0x082db9d4–0x082dbd08 | **820** | 170 |
| **H bytecode total** | | | | **90,055** | **7,549** |

Addressed but **not** H bytecode: the mystery-event 17-command VM and the
`src/mystery_gift_scripts.c` client/server script structs (0x082f25a8–
0x082f29ec, 16 symbols) — a dynamic v-address family owned by the mystery-event
engine, already native-refactored, with no battle-VM relationship. The static
mystery-gift field modules were migrated in G. The `gMysteryEventScriptCmdTable`
(17×4 B function pointers, 0x082ded2c) is an engine command table (R14
residue class E). Battle **transitions** (`battle_transition.c`,
`battle_intro.c`) are C task functions + struct/gfx tables, not bytecode —
their gfx belongs to the R13-B leaf family and their tables to R13-E-style
structured data; they are explicitly out of H.

The 21,696 B of battle message strings (`src/battle_message.c`,
`gBattleStringsTable` 369 entries) are C-owned text (§13). Trainer
intro/defeat speech is map text already owned by C and referenced from G's
`trainerbattle` seam.

### 1.2 Address-operand classification (relocation-based, exact)

| Class | battle_scripts_1/2 | battle_anim | battle_ai | contest_ai | field_effect | Total |
|---|---:|---:|---:|---:|---:|---:|
| `SCRIPT_TARGET` (in-family label) | ≈987 (926 global-label + 36 section-local + 25 bs2) | 1,152 (395 table rows + 757 command targets) | 1,190 (32 table + 1,158 internal) | 332 (32 table + 300 internal) | 67 (table rows; zero in-body branches) | **≈3,728** |
| `ENGINE_EWRAM_TARGET` (battle-struct fields: `gBattleScripting.*` 159, `gBattleCommunication*` 126, `gHitMarker` 53, `gMoveResultFlags` 39, `gBattleTypeFlags` 29, `gBattleMoveDamage*` 14, `gBattlerTarget` 12, `gCurrentMove` 10, `gBattlersCount` 6, `gBattlerAttacker` 4, `gBattleOutcome`, `gBattleMovePower`, `gBattlerAbility`, `gBattleWeather`, `gAbsentBattlerFlags`, `gBattlerFainted`, `gBattlerPositions`, `gChosenMove`, `gLastLandedMoves`, `gBattleTextBuff1/2/3`, `sDMG_MULTIPLIER`, `sMOVE_*`, `sB_ANIM_ARG1`…) | **≈480** | 0 | 0 | 0 | 0 | **≈480** |
| `ENGINE_TABLE_TARGET` (compiled `u16` string-ID tables via `printfromtable`: `gStatUpStringIds` 20, `gStatDownStringIds` 10, `gSubstituteUsedStringIds`, `gCaughtMonStringIds`, `gBallEscapeStringIds`, `gSafari*StringIds`, `gMistUsedStringIds`, `gTrainerItemCuredStatusStringIds`) | **≈81** | 0 | 0 | 0 | 0 | **≈81** |
| `ENGINE_SPRITE_TEMPLATE_TARGET` (`createsprite` → `g<Camel>SpriteTemplate`, 325 distinct, 2,108 occurrences) | 0 | **2,108** | 0 | 0 | 0 | **2,108** |
| `ENGINE_CALLBACK` (`createvisualtask`/`createsoundtask` → `AnimTask_*`, `Task_*`, `Cb_*`; 212 distinct, 962 occurrences) | 0 | **962** | 0 | 0 | 0 | **962** |
| `ENGINE_GFX_TARGET` (field-effect `loadpal`/`loadtiles` → `gSpritePalette_*` etc.) | 0 | 0 | 0 | 0 | ≈40 | **≈40** |
| `ENGINE_CALLBACK` (field-effect `callnative`) | 0 | 0 | 0 | 0 | ≈63 | **≈63** |
| Totals | 1,508+54 | 4,231 | 1,222 | 364 | 170 | **7,549** |

(The battle row carries ≈14 residual constant/misc relocations; H1 pins every
row exactly.)

Cross-checks: battle_scripts_1.o relocation histogram sums to 1,508; anim
4,231 = 2,108 + 962 + 1,152 + 6 global + 3 misc; AI/contest relocations are
100% section-relative (`R_ARM_ABS32` vs the `script_data` section symbol —
every script label in those files is a single-colon LOCAL label); battle
anim labels are likewise single-colon local (only 5 `::` symbols: the four
`gBattleAnims_*` tables + `gMovesWithQuietBGM`).

### 1.3 Decisive negative findings (prove-before-reuse, §2 of the brief)

1. **No battle→anim pointer edges.** `playanimation` (43 uses in
   battle_scripts_1.s) encodes a 1-byte anim ID; its `.int` operand is an
   EWRAM arg slot (`sB_ANIM_ARG1` = `gBattleScripting+0x10`), never an anim
   script label. The anim script is selected by the controller through
   `gBattleAnims_*` tables. The R13-A "battle script → gBattleAnims_Moves
   365" figure describes the engine-side ID-routing table, not a bytecode
   edge.
2. **No battle→text pointer edges.** Text is 2-byte `STRINGID_*` →
   `gBattleStringsTable` (C-owned skeleton, §13). The only text-adjacent
   pointer operands target compiled `u16` ID tables (ENGINE_TABLE_TARGET).
3. **No AI operand targets engine data or tables.** All 1,222 AI
   relocations are in-family label targets; every table read is C-side.
   Contest AI: same (364).
4. **No dynamic/generated bytecode in any battle VM.** No
   `gBattleScriptBuffer`/`gBattleScriptArgs` exist in this port (refactored
   VM); the anim VM has no runtime-written script buffers; AI is static.
5. **Native EWRAM does not replicate GBA EWRAM layout.** Measured:
   GBA `gBattleCommunication` 0x02024332 (offset 0x24332) vs native offset
   0x3F8 within `gba_ewram`; `gBattleScripting` GBA offset 0x24474 vs native
   0x290. A single 0x02000000→native interval mapping is therefore **invalid**
   (would resolve to the wrong objects); engine targets need per-symbol
   bindings (§8.3). This corrects the assumption that "GBA-shaped EWRAM
   addresses resolve through the interval table".
6. **No `callnative` in the anim VM**; its function operands are
   `createvisualtask`/`createsoundtask` targets. No `gBattleScriptsForStatus
   Conditions`/`gBattleScriptsForMonFormChanges`/`gBattleScriptsForBattleIntro`
   exist in this revision; status/form/intro routing is inline scripts or C.

### 1.4 Compiled routing tables holding script pointers

| Table | Entries | Width | Lives in | Consumer seam |
|---|---:|---|---|---|
| `gBattleScriptsForMoveEffects` | 214 | 4 B `GbaAddr` | battle_scripts_1.s `script_data` | `HostResolveGbaAddr(entry)` — battle_util.c:285 + 9 sites |
| `gBattlescriptsForBallThrow` / `ForUsingItem` / `ForRunningByItem` / `ForSafariActions` | 13/6/1/4 | 4 B | battle_scripts_2.s | same pattern |
| `sMoveEffectBS_Ptrs` | 38 | **8 B native host pointers**, bypasses the resolver | battle_script_commands.c:627 `.rodata` | direct read (4 sites) — the one native-width table |
| `gBattleAnims_Moves` / `_StatusConditions` / `_General` / `_Special` | 356/9/23/7 | 4 B | battle_anim_scripts.s | `LaunchBattleAnimation` (battle_anim.c:240) |
| `gFieldEffectScriptPointers` | 67 | 4 B | field_effect_scripts.s | field_effect.c |
| `gBattleAI_ScriptsTable` | 32 | 4 B | battle_ai_scripts.s | `HostResolveGbaAddr(entry)` battle_ai_script_commands.c:581 |
| `gContestAI_ScriptsTable` | 32 | 4 B | contest_ai_scripts.s | contest_ai.c:351 |
| `gBattleStringsTable` (C-owned) | 369 | 8 B | C skeleton (text seam) | battle_message.c |

## 2. Separate execution engines

Four genuinely distinct VMs; each is audited against its own interpreter, not
G's. None shares `ScriptContext` with the field VM.

### 2.1 Battle-script VM

```text
battle controllers (per-battler, per-frame)
   |
   v
gBattleMainFunc = RunBattleScriptCommands / _PopCallbacksStack
   |  (battle_main.c:5277, 5262; BattleScriptExecute battle_util.c:3201)
   v
gBattleScriptingCommandsTable[249]  (0x00–0xF8, battle_script_commands.c:329)
   |  each handler advances gBattlescriptCurrInstr itself
   |  (+= N for fixed; T1/T2_READ_PTR for pointer commands)
   v
suspension: BtlController_Emit* + MarkBattlerForControllerExec
   (bit in gBattleControllerExecFlags; blocking commands re-execute each
    frame until flags clear — IP is never advanced by controllers)
   |  action yield: Cmd_finishaction/finishturn -> B_ACTION_FINISHED
   v
call: BattleScriptPushCursor (ret = IP+5) / BattleScriptPop
   stack: gBattleResources->battleScriptsStack { ptr[8], size }  (heap)
   main-func stack: gBattleResources->battleCallbackStack { function[8] }
```

| Item | Value |
|---|---|
| Instruction pointer | `EWRAM_DATA const u8 *gBattlescriptCurrInstr` (battle_main.c:184) — serialized EWRAM slice |
| Second IP storage | `gSelectionBattleScripts[4]`, `gPalaceSelectionBattleScripts[4]` (EWRAM) — selection scripts parked across frames |
| Call stack | `BattleScriptsStack{ptr[8]}` in `gBattleResources` (heap inside EWRAM `gHeap`) |
| Callback stack | `BattleCallbacksStack{function[8]}` (same heap) |
| Opcode table | `gBattleScriptingCommandsTable[249]`, compiled engine table |
| Operand width | opcode 1 B; operands 1/2/4 B; 4-byte operands are GBA logical addresses |
| Pointer decode | `T1_READ_PTR`/`T2_READ_PTR` → `HostResolveGbaAddr` (global.h:134/140) |
| Suspension | controller-flag blocking + frame counters (`gPauseCounterBattle`); action-level `B_ACTION_*` |
| Callbacks | controllers, `gBattleMainFunc` main-loop function stack |
| RAM scripts | none (no runtime-written bytecode; all selection is pointer assignment to static scripts) |
| Serialized state | EWRAM slice (IP, scalars, `struct BattleScripting`, `gBattleCommunication`), GAME_BSS slice (`gHeap`: `gBattleResources`, `gBattleStruct`), COMMON (`gBattleMainFunc`, `gBattlerControllerFuncs`) |

### 2.2 Battle-animation VM

```text
LaunchBattleAnimation(animsTable, id)   battle_anim.c:208
   sBattleAnimScriptPtr = HostResolveGbaAddr(animsTable[id])
   gAnimScriptCallback = RunAnimScriptCommand; gAnimScriptActive = TRUE
   |
   v
per-frame drivers (battle/contest controllers, watchdog tasks) call
gAnimScriptCallback() -> do/while:
   sScriptCmdTable[48][sBattleAnimScriptPtr[0]]()
     until sAnimFramesToWait != 0 || !gAnimScriptActive
   |  frame-wait: callback swapped to WaitAnimFrameCount
   |  spin-wait: pointer held at the blocking command, re-executed next frame
   v
call/return: single ret slot sBattleAnimScriptRetAddr (no stack;
   nested call would clobber the slot — in practice subroutines are leaves)
end: Cmd_end frees sprite gfx/palettes by tag, restores BGM, Active = FALSE
```

| Item | Value |
|---|---|
| Instruction pointer | `EWRAM_DATA static const u8 *sBattleAnimScriptPtr` (battle_anim.c:92) |
| Return slot | `EWRAM_DATA sBattleAnimScriptRetAddr` (:93) — **single slot, no stack** |
| Callback | `EWRAM_DATA gAnimScriptCallback` (:94) — engine function pointer |
| Frame wait | `sAnimFramesToWait` (:95) |
| Opcode table | `sScriptCmdTable[48]` (0x00–0x2F), const |
| Pointer decode | `T2_READ_PTR` → `HostResolveGbaAddr`; functions via `HostResolveFunction` |
| Suspension | frame-wait / spin-wait; strictly sequential animations (controller flag-gated; no nesting/parallel launch from inside the VM) |
| Dynamic scripts | none |
| Serialized state | EWRAM slice (23 vars), COMMON (`gTasks`), GAME_BSS (`gHeap` buffers); script bytes never serialized |

### 2.3 Battle-AI VM

```text
per turn, per battler: OpponentHandleChooseMove ->
BattleAI_SetupAIData (zero AI_ThinkingStruct, AI_ScriptsStack.size = 0,
   derive aiFlags from trainer flags/safari/factory)
   -> BattleAI_DoAIProcessing: while (aiState != FinishedProcessing):
        SettingUp: gAIScriptPtr = HostResolveGbaAddr(gBattleAI_ScriptsTable[aiLogicId])
        Processing: sBattleAICmdTable[99][*gAIScriptPtr]()
        end (0x5A): AIStackPop; empty stack -> AI_ACTION_DONE
   -> chosenMoveId -> BtlController_EmitTwoReturnValues -> gBattleBufferA
synchronous run-to-completion within one frame; quiescent at every VBlank
```

| Item | Value |
|---|---|
| Instruction pointer | `EWRAM_DATA const u8 *gAIScriptPtr` (battle_ai_script_commands.c:154) — shared slot with contest AI |
| Call stack | `gBattleResources->AI_ScriptsStack{ptr[8]}` (heap) — reset every turn |
| Opcode table | `sBattleAICmdTable[99]` (0x00–0x62), const |
| Evaluation context | `AI_ThinkingStruct` (heap): aiState, movesetIndex, moveConsidered, score[4], funcResult, aiFlags, aiAction, aiLogicId, simulatedRNG[4]; `gBattleStruct->AI_monToSwitchIntoId[4]`, `AI_itemType/Flags[2]` |
| Suspension | none — synchronous run-to-completion |
| Dynamic scripts | none |
| Serialized state | EWRAM slice (`gAIScriptPtr`), heap stack + thinking struct; stale-but-valid IP at capture |

### 2.4 Contest-AI VM

Separate table `sContestAICmdTable[136]` (contest_ai.c:153), own interpreter
`ContestAI_DoAIProcessing` (:342), own stack `ContestAIInfo.stack[8]` inside
`gContestResources->aiData` (heap, exists only during a contest). Shares only
the `gAIScriptPtr` EWRAM slot. Reachable mid-battle capture: **no** (contest
resources are allocated/freed around the contest).

### 2.5 Field-effect micro-VM

`gFieldEffectScriptFuncs[8]` (field_effect.c:278) over 67 tiny scripts:
`loadtiles`/`loadfadedpal`/`loadpal` (gfx pointers), `callnative`/`loadgfx_
callnative`/`loadtiles_callnative` (engine C functions), `end`. Pure
engine-target bytecode with zero in-family branches; a leaf-shaped family.

## 3. Grammar audit

All five languages use 1-byte opcodes without length bytes; operand layout is
per-command fixed. H1 emits the machine-readable grammar
`{opcode, name, fixedSize|dynamicRule, operands[], flow, maySuspend}` per VM
(the G1 extractor pattern, one descriptor per VM).

| VM | Commands | Pointer-bearing commands (`.int` operands) |
|---|---|---|
| Battle | 249 | `goto`, `call`, `accuracycheck` (failPtr), `tryfaintmon_spikes`, `jumpifstatus/2/3condition` (status mask **and** target), `jumpifability`, `jumpifsideaffecting`, `jumpifstat`, `jumpiftype`, `jumpifaffectedbyprotect`, `jumpiftype2`, `jumpifabilitypresent`, `jumpifcantswitch`, `jumpifplayerran`, `jumpifnexttargetvalid`, ~50 `try*`/`*calculator` effect commands (jump target), `jumpifbyte/halfword/word` (engine addr **and** jump), `jumpifarrayequal/notequal` (2 addrs + jump), `setbyte/addbyte/subbyte/orbyte/bicbyte/orhalfword/bichalfword/orword/bicword` (engine addr), `copyarray`/`copyarraywithindex` (2–3 addrs), `printfromtable`/`printselectionstringfromtable` (engine table), `playanimation` (EWRAM arg slot), `playanimation_var` (2 EWRAM slots), `tryfaintmon` (NULL constant), `decrementmultihit` (scalar) |
| Anim | 48 | `createsprite` (template), `createvisualtask`/`createsoundtask` (function), `call`/`goto`/`jumpifcontest`/`jumpifmoveturn`/`jumpargeq` (script), `choosetwoturnanim` (2 script) |
| Battle AI | 99 | `call` (0x58), `goto` (0x59), `if_*` conditional jumps (0x00–0x03 random, `if_level_cond` 0x5B, …) — all script targets |
| Contest AI | 136 | `call`, `goto`, `if_*` — all script targets |
| Field effect | 8 | `loadtiles`/`loadfadedpal`/`loadpal` (gfx), `callnative` + variants (functions) |

Typed operand classes (the G vocabulary, extended):

```
SCRIPT_TARGET            in-family label (binding + interior offset)
ENGINE_EWRAM_TARGET      compiled EWRAM engine field (per-symbol binding, §8.3)
ENGINE_TABLE_TARGET      compiled numeric u16 ID table (per-symbol binding)
ENGINE_SPRITE_TEMPLATE_TARGET  compiled C SpriteTemplate global (per-symbol)
ENGINE_CALLBACK          compiled C function (HostResolveFunction binding)
ENGINE_GFX_TARGET        compiled gfx array (R13-B family bridge, field-effect)
RAW_DATA_TARGET          (none found in H families)
TEXT_TARGET              (none — text is ID-routed, §13)
```

No raw 32-bit value is classified as a pointer by shape: class assignment is
proven per operand from the handler implementation, the emitting macro, the
qualified ELF relocation (symbol + addend), and the qualified ROM bytes.
Battle operands carry nonzero addends (`gBattleCommunication + 1`,
`gBattleMoveDamage + 1..3` via `setword`'s four `setbyte`s) — the relocation
record stores symbol + addend, not just an address.

## 4. G-architecture reuse comparison

| G mechanism | Battle | Anim | AI / contest AI | Verdict |
|---|---|---|---|---|
| Exact bytecode + relocation sidecar | yes (1,562 operands) | yes (4,231) | yes (1,222 / 364) | **reuse** — identical shape |
| Module resources | per-root with shared-tail owner assignment (§5) | same | same (or per-file — trivially safe) | **reuse with per-VM granularity proof** |
| Source relocation index (operand address → row) | yes | yes | yes | **reuse** |
| Target binding index (GBA provenance → live target) | yes (script targets) | yes | yes | **reuse** |
| Reverse containment | yes | yes | yes | **reuse** |
| State-v5 key+offset sidecar | yes | yes | yes | **reuse, no format change** |
| Atomic generation publication | yes | yes | yes | **reuse** (three-phase) |
| G's typed resolver over `ScriptReadPointer` | **not mechanically** — the battle VM has ~25 pointer-consuming handlers + ~20 table-entry reads spread over battle_script_commands.c/battle_util.c/battle_main.c/battle_setup.c, with **engine-target operands** G never had | 9 handlers + 1 launch path, but operands include engine templates/functions | 2 readers (table read + `call`/`goto`/`if_*` advance) | **one per-VM typed reader**, shared resolver backend (§8) |
| G's "0 nonzero addends" assumption | **false** — battle engine-data operands carry addends | templates/functions: symbol-addressed (no addends) | n/a (section-relative) | extractor must record addends |
| G's logical-registry mapping | **insufficient** — GBA EWRAM layout ≠ native layout (§1.3.5) | n/a | n/a | H adds a per-symbol engine-binding table |
| G's 523 per-module State-v5 ranges | would add ≈2,060 ranges → 8,437 > 8,192 cap | | | **register one range per family arena** (5 ranges; §18) |

The G resource/relocation/runtime model is reused **per VM** with three
documented extensions: (a) an `ENGINE_*` target class resolved through a
generated per-symbol engine-binding table (no registry-capacity or layout
assumptions), (b) addend-aware relocation records, (c) family-arena range
registration.

## 5. Interior-target audit and granularity

- Battle: 617 roots in one file; 26 local labels (shared sub-entry points);
  cross-root edges are the norm (137 edges to `BattleScript_MoveEnd` alone,
  95 to `BattleScript_ButItFailed`); ≈987 script-target operands ≈ 300
  distinct targets, ~all interior offsets. Return addresses (`BattleScriptPush
  Cursor` = IP+5) are interior offsets by construction.
- Anim: ≈657 labels in one file, all local; 757 command script targets
  (call/goto/jumpifmoveturn/jumpargeq/jumpifcontest/choosetwoturnanim) + 395
  table rows; shared sub-scripts (252 bare camelCase labels like
  `BallThrowEnd`, `DoubleSlapContinue`) are called from many parents.
- AI: 1,190 internal control-flow targets, all local labels; 32-table-row
  roots; 12 helper labels (`CheckIf*`). Contest: 300 internal + 32 rows;
  cross-root `call`s exist (3 in contest_ai_scripts.s).
- Field effect: zero interior branches (leaf scripts).

**Decision (per-VM, proven from the geometry):**

- **Battle scripts: per-root graph-aware modules** (617 + 22 = 639 resources).
  Each root owns its label span; the 26 shared local labels are assigned to
  their containing root by the deterministic G rule ("smallest unique source
  module"; overlapping candidates ⇒ refusal), and other roots bind to the
  owner's export. Cross-root edges become binding relocations — the same
  mechanism G uses for map-to-common edges. Per-symbol resources are invalid
  here for exactly G's reason: 1,562 operands include hundreds of interior
  offsets and IP+5 return addresses that per-symbol identity cannot contain.
- **Anim scripts: per-root modules** (≈657, minus the 5 tables) with the same
  shared-tail owner rule (the 252 shared sub-scripts are the common case).
- **AI scripts: per-root modules** (533 battle + 164 contest) — or one
  per-file module; the per-root choice is preferred for mod granularity and
  costs nothing because AI edges never cross roots except the 3 contest
  `call`s (binding edges).
- **Field effect: one module per script** (67) — leaf-shaped.

Module spans are typed segments within one deterministic per-family arena;
text/gfx holes are external bindings (none exist inside H payloads except
none at all — H bytecode contains no C-owned payload interleaved, unlike
event_scripts.s).

## 6. Canonical resource identities

Keys follow the M0/M1 rule and the G naming convention, per-VM namespaces:

```text
emerald:battle-script/<root>            (BattleScript_*, 639 + routing)
emerald:battle-anim-script/<label>      (Move_*/General_*/Special_*/Status_*/
                                         Frustration_*/bare labels, ≈657)
emerald:battle-ai/<root>                (AI_* battle, 533 + table)
emerald:contest-ai/<root>               (AI_* contest, 164 + table)
emerald:field-effect-script/<label>     (gFieldEffectScript_*, 67)
```

Root names are lower-case/dash-normalised from the semantic label (not the
raw GBA address). Each resource exports bindings `...#<label>` with semantic
label, original GBA address, module-relative provenance offset, packed-payload
offset, boundary kind, and aliases (duplicate labels at one address; e.g.
`AI_CheckBadMove` exists in both the battle-AI and contest-AI families —
distinct keys, unambiguous). Anonymous/shared labels become
`#<label>` inside their owner module. Routing tables (the 13 pointer tables)
are typed segments owned by their family's routing module
(`emerald:battle-script/routing` etc.) — G's map-dispatch precedent —
or standalone resources where mods would replace a table row (recommended:
rows are overridable bindings).

## 7. Canonical pack representation

**Exact original encoded bytecode + sidecar metadata; no native pointer
patching; no operand widening — viable for all five families.** Verified
facts: every operand is 4 encoded bytes (no VM embeds 8-byte values in
bytecode); the sole native-width script table (`sMoveEffectBS_Ptrs`,
38 × 8 B) is an engine-side table rebuilt from bindings at publication, not
a payload form; anim sprite-template/function operands are 4-byte slots
resolved through the engine-binding table. The pack record mirrors G:

```text
module key, schema, exact-byte digest
segments[] = {kind, originalGbaStart, byteCount, payloadOffset}
exports[]  = {name, originalGbaAddress, provenanceOffset, payloadOffset,
              boundaryKind, aliases[]}
relocs[]   = {operandPayloadOffset, operandWidth=4, originalEncodedGba,
              targetClass, targetIdentity, runtimeResolutionRequired}
```

`targetIdentity` is: `{resourceKey, export | interiorOffset}` for
`SCRIPT_TARGET`; `{engineSymbol, addend}` for every `ENGINE_*` class. All
operands retain their original encoded four bytes; resolution happens at
consumption (§8). The engine-binding census (battle ≈25 distinct EWRAM
symbols + ≈12 ID tables; anim 325 templates + 212 functions; field-effect
≈20 palettes + ≈10 functions) is a generated table of
`{gbaAddress | gbaAddress+addend → nativeAddress}`, emitted from the
qualified ELF + the native link — never hand-maintained, never serialized.

## 8. Runtime resolution model

### 8.1 One typed reader per VM over one shared resolver backend

Each VM keeps its opcode values, stack ABI, and grammar unchanged. The
pointer-consuming handlers are moved to a per-VM typed reader with the same
shape as G's `ScriptReadPointer` replacement:

```text
ResolveTargetIdentity(location, encodedU32) -> typed live pointer
  SCRIPT_TARGET              -> source index row -> arena base + bound offset
  ENGINE_EWRAM_TARGET        -> engine-binding table[engineSymbol] + addend
  ENGINE_TABLE_TARGET        -> engine-binding table (compiled native table)
  ENGINE_SPRITE_TEMPLATE_TARGET -> engine-binding table (compiled template)
  ENGINE_CALLBACK            -> engine-binding table / HostResolveFunction
  ENGINE_GFX_TARGET          -> R13-B leaf binding (field effect only)
```

### 8.2 Exact code-change budget

- **Battle VM:** one central reader replacing `T1/T2_READ_PTR` in the
  pointer-consuming `Cmd_*` handlers (≈25 sites in
  battle_script_commands.c, all already funnel through the two macros), the
  ≈20 `gBattlescriptCurrInstr = <table-entry/symbol>` assignment sites
  (battle_util.c, battle_main.c, battle_script_commands.c) moved to a
  publish-table read, `sMoveEffectBS_Ptrs` republished from bindings (38
  slots), and the 3 `sTrainerBattle*ScriptRetAddr` surfaces (already
  modeled as G pack slots — SURFACE_TRAINER_*; H only widens their class
  to allow `BATTLE_SCRIPT_TARGET`). The 249-command dispatch, stack, and
  controller protocol are untouched.
- **Anim VM:** the 9 pointer commands (`createsprite`, `createvisualtask`,
  `createsoundtask`, `call`, `goto`, `jumpifcontest`, `jumpifmoveturn`,
  `jumpargeq`, `choosetwoturnanim`) + `LaunchBattleAnimation`'s table read
  (battle_anim.c:240) + the status-anim launch path. Nothing else.
- **AI VMs:** `gBattleAI_ScriptsTable` read (battle_ai_script_commands.c:581),
  `gContestAI_ScriptsTable` read (contest_ai.c:351), and the `call`/`goto`/
  `if_*` target advance in both interpreters.
- **Field effect:** the 6 pointer commands' operand reads.

No broad interpreter rewrites. `HostResolveGbaAddr` itself is unchanged; the
H seams layer above it exactly as `EmeraldScriptCompat` does today.

### 8.3 Engine-EWRAM binding (the H-specific element)

Because native EWRAM layout ≠ GBA EWRAM layout (§1.3.5), the 462
`ENGINE_EWRAM_TARGET` operands resolve through the generated per-symbol
binding table, keyed by the qualified relocation's (symbol, addend). The
table is engine-owned, compiled, and generated deterministically (native
`nm` cross-checked against the qualified ELF census; drift = refusal).
Struct layouts themselves are shared GBA/native (same headers), so only
the section placement differs — per-symbol binding sidesteps it entirely.
No State-v5 interaction: these operands are transient reads; no engine-data
pointer enters a serialized pointer field through them.

## 9. Battle-script VM specifics

- IP: `gBattlescriptCurrInstr` (EWRAM). Stack: `battleScriptsStack{ptr[8]}` +
  callback stack `battleCallbackStack{function[8]}` (heap). Call = push
  IP+5 (battle_util.c:966); return = pop; `end3` unwinds both stacks +
  `gBattleMainFunc`.
- Dispatch: 249 handlers advance the IP themselves; blocking commands
  re-execute until controller flags clear — the parked IP is always a
  command START (the boundary class for capture).
- Shared/common scripts: `BattleScript_MoveEnd`, `_ButItFailed`,
  `_EffectHit`, `_PrintMoveMissed`, `_HitFromAtk*` — ordinary roots with
  heavy inbound edges (binding relocations).
- Trainer handoff from G: `trainerbattle` (scrcmd.c:1931) →
  `BattleSetup_ConfigureTrainerBattle` → `sTrainerBattle*` statics →
  `ScriptContext_Stop()`; post-battle return via `gMain.savedCallback`,
  `ScriptContext_Enable()`, and `gotopostbattlescript`/`gotobeatenscript`.
  These are G-owned surfaces today; H's only obligation is that the three
  continuation statics (already State-v5 pack slots) keep accepting
  battle-script targets and that `TrainerBattleLoadArg*` resolves them.
- Text: `printstring` 2-byte ID → `PrepareStringBattle` →
  `gBattleStringsTable` (C-owned, §13). `printfromtable` → compiled u16 ID
  tables (engine-binding table).
- Anim handoff: `Cmd_playanimation` (battle_script_commands.c:4009) reads
  the EWRAM arg slot; the controller resolves the 1-byte anim ID through
  `gBattleAnims_*` (engine-side, §12).

## 10. Battle-animation VM specifics

- Single-file family, 63,812 B; 48 commands; single return slot (no stack);
  strict sequential animations; no dynamic scripts.
- Sprite templates: 325 distinct compiled C globals (engine-owned, stay
  compiled) — operands are 4-byte slots resolved via the engine-binding
  table; the templates' inner gfx/palette pointers already point at
  R9-pack-owned payloads through the compat tables.
- Resources by ID: `ANIM_TAG` (16-bit) → `gBattleAnimPicTable[289]`/
  `gBattleAnimPaletteTable[289]`; `BG_*` (1-byte) → `gBattleAnimBackground
  Table[27]`; SE IDs (16-bit) → R12 audio. All ID-routed, zero bytecode
  pointer edges into gfx/audio.
- State-v5: 23 EWRAM vars + task slots; script bytes never serialized;
  capture may park the IP at a blocking command (`waitforvisualfinish`,
  `waitbgfade*`, `end`) — boundary class INSTRUCTION_START.
- All scripts static; mon animations (pokemon_animation.c) and battle intro
  are separate sprite-callback systems, out of H.

## 11. Battle-AI specifics

- `gAIScriptPtr` (EWRAM, shared with contest AI) + `AI_ScriptsStack{ptr[8]}`
  (heap, reset per turn) + `AI_ThinkingStruct` (heap).
- Synchronous run-to-completion within the controller callback: at every
  VBlank boundary the VM is quiescent (aiState == FinishedProcessing) with a
  stale-but-valid `gAIScriptPtr`. Capture policy: relocate the stale pointer
  (sidecar record, boundary-validated) even though the next turn re-derives
  it — never leave an uncontained creator-process pointer in EWRAM.
- Data reads: all C-side (gBattleMoves/gSpeciesInfo/gTrainers — already
  ROM_BASE pack-owned by R13-D/E; gTypeEffectiveness still compiled,
  untouched by H).
- Contest AI: separate table/stack, only shares the `gAIScriptPtr` slot;
  contest resources never exist during a battle capture.
- Multiple banks: one 32-entry table per family; script selection by
  trainer `aiFlags` bit loop (battle) / contest class (contest).

## 12. Cross-VM edges (typed handoff map)

```text
G field script ─trainerbattle special─► battle setup (engine state:
   sTrainerBattle* continuation statics — already G State-v5 slots)
battle VM ─playanimation─► controller ─1-byte B_ANIM id─► gBattleAnims_*
   (H routing table) ─► anim VM          [ID-routed, no bytecode pointer]
battle VM ─printstring─► controller ─2-byte STRINGID─► gBattleStringsTable
   (C-owned skeleton)                    [ID-routed]
battle VM ─printfromtable─► compiled u16 ID tables   [engine-binding row]
battle VM ─setbyte/jumpifword/...─► EWRAM engine fields [engine-binding row]
battle VM ─goto/call─► BattleScript_* roots           [H arena binding]
anim VM ─createsprite─► compiled SpriteTemplate globals [engine-binding row]
anim VM ─createvisualtask/soundtask─► AnimTask_* C funcs [engine-binding row]
anim VM ─call/goto/jumpargeq/...─► in-file labels      [H arena binding]
AI VM ─call/goto/if_*─► in-file AI labels              [H arena binding]
AI VM ─► gBattleMoves/gSpeciesInfo (R13-D/E pack), gBattleStruct (EWRAM)
```

Every edge is a resource reference, engine state, callback, or scalar ID —
no raw cross-VM pointer ambiguity exists in the bytecode (the audit's
decisive finding). The G→battle→G trainerbattle boundary remains
engine-state-typed exactly as G6 established it.

## 13. R13-C text handoff

Battle text is **already C-owned and live**: `gBattleStringsTable[369]` is a
C skeleton table (`text_skeleton_arrays.generated.c:138`, NULL at link) filled
by the text seam from `emerald:text/battle/*` identities (522 battle text
identities in `text/ownership.generated.toml`; the catalog also covers
move/ability/nature skeleton fills). Battle bytecode references text only by
2-byte ID — **zero text-pointer relocations in any H family** (verified in
the qualified relocation census). Trainer intro/defeat speech resolves
through G's trainerbattle seam (C text via G sidecar). `printfromtable`
targets are numeric u16 ID tables (no text payload, no C identity needed).

Result: **zero additive C prerequisite for H.** H must not duplicate any
text bytes; the C arena and skeleton republish are untouched.

## 14. State-v5 surfaces

No State-v5 format change. All VM state already lives in serialized slices;
after cutover the script-holding fields switch from image-relative to
sidecar records (the G4-proven pattern):

| Surface | Slots | Slice | Stable form |
|---|---:|---|---|
| `gBattlescriptCurrInstr` | 1 | EWRAM | H key + instruction offset (boundary INSTRUCTION_START) |
| `battleScriptsStack.ptr[]` | ≤8 | GAME_BSS (`gHeap`) | H key + next-instruction offset (NEXT_INSTRUCTION for IP+5 returns) |
| `battleCallbackStack.function[]` | ≤8 | GAME_BSS | engine function image (existing handling, unchanged) |
| `gSelectionBattleScripts[]` / `gPalaceSelectionBattleScripts[]` | ≤8 | EWRAM | H key + offset |
| `gAIScriptPtr` | 1 | EWRAM | H key + offset (quiescent stale IP at capture) |
| `AI_ScriptsStack.ptr[]` | ≤8 | GAME_BSS | H key + offset (size 0 at VBlank capture; validated anyway) |
| `sBattleAnimScriptPtr` / `sBattleAnimScriptRetAddr` | 2 | EWRAM | H key + offset |
| `gAnimScriptCallback` | 1 | EWRAM | engine function (existing special-case) |
| `sTrainerBattleEndScript` + A/B returns | 3 | EWRAM | already G slots — class widened to accept H targets |
| task `data[]` anim fields, sprite slots | — | TASK/SPRITE sidecars | engine/sprite identity (unchanged) |

Worst-case mid-battle record count ≈ 1 + 8 + 4 + 1 + 0 + 2 + 3 ≈ **19**;
with selection scripts ≈ 27 — versus the 4,096 sidecar cap (≈1,005 audio +
≈33 field-script today). No cap change.

## 15. Save-legality policy

- **In-game SaveBlock save** (`SaveGame`, start_menu.c:1017): reachable only
  through the overworld start menu, never during battle — no battle VM state
  can be captured by this path. Battle Tower/link saves occur between
  battles. **No H work required for this path.**
- **Native quick-save (F5) / manual save (F7) / state manager (F6)**: the
  capture is unconditional (no battle gate, sdl2.c:1804+) and pauses the
  worker at a VBlank boundary — **mid-battle, mid-animation, and mid-AI-turn
  capture are first-class supported paths**. Policy per VM:
  - Battle VM: full support — suspended blocking commands park the IP at a
    command START; capture/restore must relocate IP, both stacks' script
    slots, and selection-script parking slots (mandatory fresh-process
    cases in H3).
  - Anim VM: full support — IP/ret slot relocated; frame-wait/spin-wait
    counters are scalars (already serialized).
  - AI VM: capture may contain a stale-but-valid IP; relocate it, do not
    execute it (the next turn re-derives). No capture refusal is needed for
    any H VM (unlike G's Context2) because every H VM parks at stable
    boundaries by construction.
  - Contest AI: never exists during a battle capture; mid-contest captures
    get the same treatment as battle AI (per-contest sidecar).

No State-v5 format change and no new slice are needed for any of this.

## 16. Publication seams

Recommendation: **shared internals, three independent family seams.**

- `EmeraldBattleScriptCompat` — battle + item-routing tables (13,596 B).
- `EmeraldBattleAnimCompat` — anim (63,812 B) + field-effect (820 B).
- `EmeraldBattleAiCompat` — battle AI (9,303 B) + contest AI (2,524 B).
- Shared: the H generator/extractor family (grammar descriptors per VM),
  the arena/relocation core (same sidecar schemas and indexes as G),
  the engine-binding census generator, and the State-v5 family adapter.

Each family seam runs the G three-phase transaction (resolve/validate →
stage → commit) with its own generation; boot ordering is
`B/C/G arenas → H arenas (any order) → first battle entry`, with
dependency-free cutovers (§17). No H stage may force all three families into
one transaction; a failed family generation refuses startup or preserves the
prior complete generation, exactly like G.

## 17. H subdivision

Seven gated waves, mirroring the proven G shape. The three live-cutover
waves are mutually independent (no cross-family bytecode edges exist), so
each is a safe standalone gate:

- **H1 — grammar, graph extractor, engine-binding census.** Machine grammar
  for all five VMs; exact segment/relocation/binding graphs; the addend-aware
  engine-target census (battle 462 EWRAM + ≈42 tables; anim 2,108 templates
  + 962 functions; field-effect ≈170); all §1 pins reproduced by an
  independent three-way oracle. Gate: zero unresolved/ambiguous operands,
  all counts match. No live consumer.
- **H2 — additive module resources.** Generate ≈2,056 per-root modules +
  5 routing modules + typed sidecars + the engine-binding table. Gate:
  deterministic pack, three-way ROM/ELF/resource parity, zero missing
  dependencies, still no live cutover.
  **COMPLETE (2026-08-22):** 2,089 modules / 2,081 payload / 90,047 B /
  7,549 relocs / 3,798 SCRIPT_TARGET + 3,751 ENGINE / 718 binding rows /
  65 NULLs / 3,058 addends. Pack entries reconciled **23,077 → 23,069**
  (§18 arithmetic: the 8 zero-width alias modules carry no pack records;
  pack = 20,988 + 2,081). Pack deterministic (SHA-256
  `b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb`).
  Report: `docs/R13H2_BATTLE_SCRIPT_RESOURCE_REPORT.md`. Live range count
  unchanged at 6,377 (zero H ranges); ownership 2,089 ×
  COMPILED_PENDING_MIGRATION; nothing staged live.
- **H3 — State-v5 execution readiness.** Dry-run capture/restore harness at
  forced-different arena bases: battle IP + nested call stack + blocking
  command; anim parked IP + ret slot; stale AI IP; selection-script parking.
  **STOP gate for H4:** a nested battle-script save whose IP and at least
  two stack returns are interior offsets must restore in a fresh process at
  a forced-different arena base and execute the exact next canonical opcode.
  Live ranges stay at 6,377 (zero H ranges until cutover).
  **COMPLETE (2026-08-22):** the STOP gate is green — a mid-battle save
  parked at `waitmessage` (0x12) with two IP+5 stack returns in different
  modules captures through the ordinary State-v5 sidecar and restores in a
  fresh process at a forced-different arena base with a perturbed physical
  layout, executing the exact next canonical opcode and return sequence.
  Five-family range dry run 6,377 → 6,382 proven (scratch index, rollback,
  identity splice); zero-width aliases canonicalize to the payload owner;
  fault matrix 16/16; G State-v5 regressions green; no format change; no
  interpreter TU modified. Report:
  `docs/R13H3_BATTLE_STATE_READINESS_REPORT.md`.
- **H4 — anim + field-effect live cutover.** Smallest state surface, zero
  dependencies on other H families, engine targets stay compiled. Gate:
  stepwise differential oracle (48 + 8 opcode slots), full battle/contest
  world animation flows, sanitizers.
  **COMPLETE (2026-08-23):** both families execute from the H arena through
  the production live seam (`emerald_battle_live.c` +
  `battle_live_table.generated.c`), published transactionally by the R6
  loader (validate anim → validate FE → stage → register ranges → publish →
  enable execution → commit; fail-closed full rollback). Live ranges
  **6,379 / 8,192** (6,377 + exactly 2 H ranges; STOP condition not hit).
  726 modules (723 payload + 3 zero-width aliases): anim 650 BYTECODE + 5
  ROUTING + 3 aliases = 658, FE 67 BYTECODE + 1 ROUTING = 68; 717
  launchable roots; 4,401 relocs (anim 4,231: 1,152 SCRIPT_TARGET / 2,108
  SPRITE_TEMPLATE / 963 CALLBACK / 8 TABLE; FE 170: 67 SCRIPT_TARGET /
  67 CALLBACK refuse-if-unknown / 36 GFX semantic); 620 bindings, 4
  refuse-only; 9,818 boundaries; 64,628 canonical bytes; 2 layouts.
  Oracle byte-exact on both layouts for all 723 modules; fault matrix
  11/11; replacement proof (6,379 invariant, identity-specific
  unregister); fresh-process anim State-v5 proof green (mandatory §15);
  anim interpreter cut over at LaunchBattleAnimation + 9 command sites,
  FE at FieldEffectStart + 4 operand helpers (void→bool8); pack invariant
  23,069; ownership stays COMPILED_PENDING_MIGRATION (658/658, 68/68); no
  compiled payload removed. Report:
  `docs/R13H4_ANIMATION_FIELD_EFFECT_LIVE_CUTOVER_REPORT.md`.
- **H5 — battle VM live cutover.** IP/stack/heap state, engine-EWRAM
  bindings, `sMoveEffectBS_Ptrs` + routing-table republication, trainer
  handoff edges. Gate: stepwise oracle over the 249-opcode table (real +
  synthetic fixtures), trainer/safari/tower/palace/link flows, the G6
  battery, no compiled battle-script fallback.
- **H6 — AI + contest AI live cutover.** Gate: AI stepwise oracle
  (opcode, score[], aiState, control flow, chosen move), contest flows.
- **H7 — compiled H removal and isolation.** Remove all five families from
  the linux64 link (assembly gates per data file), flip ownership to
  `ROM_BASE_ONLY` (GBA stays COMPILED), run the full isolation sweeps
  (§21), full regression battery, and the fresh-process State-v5 suites.
  Gate: every H resource ROM_BASE_ONLY, 0 pending, documented
  engine/ID-table exclusions present and named, **no R13-I begun**.

Each wave produces a permanent artifact; none introduces patched bytecode,
a temporary native dialect, or a live state gap.

## 18. Resource count / pack arithmetic

| Quantity | Today (post-G) | Post-H | Cap | Verdict |
|---|---:|---:|---:|---|
| Pack entries | 20,988 | **23,069** (H2: +2,081 payload records; the 8 zero-width alias identities and routing metadata are not pack records — plan's ≈23,049 and brief's 23,077 both reconcile to 23,069) | 32,768 | headroom ✓ |
| Live State-v5 ranges | 6,377 | ≈ **6,382** (one range per family arena: battle, anim, AI, contest, field-effect) | 8,192 | headroom ✓ |
| State-v5 sidecar records | ≈1,005 audio + ≈33 field worst case | + ≈27 mid-battle worst case | 4,096 | headroom ✓ |
| Canonical bytes into pack | — | ≈ **90.1 KB** (90,055 total; the routing-table rows are already inside the per-family object spans) | — | — |

**Range-count decision:** per-root range registration (G's granularity)
would need ≈2,060 ranges → 8,437 > 8,192, exceeding the cap. H therefore
registers **one range per family arena** (5 ranges). Capture correctness is
unaffected: the sidecar record (key, rangeOffset) resolves through the
family arena exactly as G's records do; the exact-module identity lives in
the seam's reverse-containment index (per-root spans) for validation and
diagnostics, and provider-composition changes are rejected wholesale by the
session fingerprint (the standing rule). The fallback — raising the range
cap to 16,384 and registering per-root ranges — is documented as an option
with this arithmetic, not exercised now. The per-module-span containment
guarantees (boundary proofs, hull checks) work identically at either
granularity.

## 19. Reuse of generic G infrastructure

Recommendation: **B (extract a bounded generic core before H) for the
generator; C (parallel sibling, no live-seam refactor) for the runtime.**

- **Extract before H (bounded):** the script-family generator core
  (`tools/gen3_resources/script_family/`) becomes parameterized per VM —
  grammar descriptor + macro-file path + operand-class rules. The sidecar
  schema, the source/target/boundary/reverse-containment index builders,
  the three-phase arena machinery's pure parts, the State-v5 adapter
  contract, and the fault-injection helpers are shared verbatim. This is a
  generalization pass on already-proven code, not a rewrite; G's generated
  artifacts must regenerate byte-identically (`--check`) throughout.
- **Keep separate until R14 (C):** the live `EmeraldScriptCompat` seam is
  frozen and proven; H implements sibling seams (own TUs, same headers and
  schemas) rather than refactoring G mid-flight. A later R14/Tallgrass
  cleanup can merge the seams; doing it during H risks the G battery for no
  runtime benefit.
- **Reuse as-is (A):** pack format, resource schemas, the State-v5 sidecar
  format, the range-index core, the session fingerprint, `HostResolveGbaAddr`
  (unchanged — H layers above it), and the isolation runner's sweep grammar.

## 20. Differential / oracle strategy

Per-VM stepwise dual-run harnesses (compiled baseline vs resource
generation, one command per step, identical cloned state) comparing opcode,
pre/post canonical IP, stack depth + canonical return offsets, VM scalars
(score[], aiState, args, flags, damage/vars), resolved target identities
(by canonical key, never host pointer equality), controller emissions, and
suspension/resume points.

- Battle: real + synthetic fixtures covering all 249 opcode slots
  (including the ≈50 unused `try*` slots, `decrementmultihit` scalar,
  `tryfaintmon` NULL operand, `jumpifarrayequal` addend forms), all
  blocking-command suspensions, max 8-deep call nesting, trainer/safari/
  tower/palace full-battle flows through the G trainerbattle handoff, and
  post-battle return to the field.
- Anim: all 48 slots; sprite/task effects compared by tag/ID and task
  state; call/ret single-slot semantics; every `gBattleAnims_*` entry.
- AI: all 99 battle + 136 contest slots; score[4]/aiState/control flow/
  chosen-move implications; script stack contents.
- Malformed fixtures per VM: truncated pointer command, bad boundary,
  unknown opcode, stack overflow (8+1) — all refuse.
- State-v5 legs: fresh-process restore at forced-different arena bases for
  the H3 mandatory cases.

## 21. Isolation strategy (H7)

Prove per family, on fresh -B release and DINFO binaries:

- compiled battle/anim/AI/contest/field-effect bytecode absent (symbol
  sweeps: 639 `BattleScript_*` + 30 local labels / ≈657 anim / 697 `AI_*`
  (battle + contest) / 67 `gFieldEffectScript_*`, plus every local label
  via the section scan);
- compiled routing-table payloads absent (the 13 tables' rows);
- runtime execution only from H arenas (IP/stack/task surfaces in-arena or
  approved engine images);
- no compiled fallback path (boot refuses when H resources are missing —
  the `TestLoaderRefusesScriptMissingPack` pattern);
- C text stays C-owned (gBattleStringsTable skeleton republish unchanged);
- engine callbacks/tables remain engine-owned (named allowlists: the 249/48/
  99/136/8 command tables, `sMoveEffectBS_Ptrs` slots, 325 sprite templates,
  212 anim functions, the ≈30 battle engine-EWRAM symbols, the u16 ID
  tables, `gTypeEffectiveness`).

No broad section exemptions; the per-family arena bytes are swept with the
G6 window grammar, and sub-1 KB rows fall under the existing NOTE rule.

## 22. ROM-hack implications

With per-root modules + typed sidecars, an eventual importer/mod system can:

- replace one battle script: override `emerald:battle-script/effect-hit`
  with a new module; callers (bytecode gotos and the move-effect routing
  rows) resolve through the composed binding table — the G §16 mod model
  verbatim;
- alter one AI script: same, per-root override; score tables unchanged;
- replace one animation script: override `emerald:battle-anim-script/
  move-thunderbolt`; the `gBattleAnims_Moves` row is a binding override;
- add new interior labels/branches: new labels become bindings inside the
  replacement module; the pack composer recomputes offsets and sidecars
  (provenance-independent);
- override cross-family bindings: ID-routed edges (anim IDs, string IDs)
  are table-row overrides — a mod changing an anim's ID routing edits the
  routing module's row, not any bytecode.

No importer/mod loader is implemented by H.

## 23. Highest-risk problem and STOP gate

The hardest single problem is **mid-battle state relocation across a
suspended blocking command with a nonempty call stack**: a save captured
while `Cmd_waitmessage`/`Cmd_healthbar_update` is blocked parks the IP at
that command's start, the 8-deep battle stack holds IP+5 return offsets
(interior offsets in arbitrary modules), the callback stack holds main-func
entries, and the controller's in-flight message lives in the BATTLE_SIDECAR.
The failure mode is the G one, sharpened: restore appears successful, then
the controller clears its flag and the blocking command advances — from the
wrong module or offset — corrupting the battle state silently.

Containment: per-root modules with boundary maps (blocking-command IPs are
provably INSTRUCTION_START; stack returns NEXT_INSTRUCTION), the typed
sidecar, reverse containment, and the G4-proven State-v5 machinery — plus
the H3 nested-case gate. No new state format is needed.

**STOP gate:** H4 may not start until H3 demonstrates a mid-battle save
whose IP is parked at a blocking command and at least two stack returns are
interior offsets, restored in a fresh process at a forced-different arena
base, executing the exact next canonical opcode and return sequence. No
other STOP-level architectural blocker was found: no cross-VM pointer
ambiguity, no State-v5 format change, no range/sidecar cap breach, and the
engine-target operands resolve through a bounded generated binding table.

## 24. Relative complexity

Driven by interpreter/state/graph surface, not byte count:

| Scope | Complexity |
|---|---|
| R13-C text | 1.0 (baseline) |
| R13-G field scripts | 3.0–4.0 (16,704 operands, sAddressOffset, split context state, F/B/C seams) |
| Battle scripts (H5) | **2.0–2.5** — 1,562 operands; no v-address family; engine-EWRAM binding table is new but bounded; richer suspension (controller flags) but all state already serialized |
| Anim scripts (H4) | **1.2–1.5** — 4,231 operands but no stack, single ret slot, engine targets stay compiled; simplest cutover |
| AI scripts (H6) | **0.8–1.0** — all-local control flow, synchronous execution, quiescent at capture |
| Contest AI / field effect | **0.5** — leaf shapes |
| **H overall** | **2.5–3.5** — several small VMs; lower than G's estimate (4.0–6.0) because the cross-VM pointer graph G assumed does not exist |

## 25. STOP conditions

STOP an H wave if: a migrated payload remains compiled in release or DINFO
without a documented structural exception; a live IP/stack pointer falls
back to removed compiled bytecode; any PCM/state delta appears in the
release flavor; State-v5 ranges/hulls/counts change unexpectedly; startup
with a valid pack fails; invalid/missing pack crashes instead of refusing;
the GBA data path changes in a new way; an engine rewrite is required
(opcode changes, widening, bytecode patching); any prior R13 regression
remains unresolved (full battery incl. movement/leaf and audio runners);
or the H3 STOP gate is not green before the first live cutover. H must not
change any resource identity, State-v5 format, or provider precedence.

STOP. This document does not implement R13-H, modify production code,
commit, or begin R13-I.

## 26. Confirmed by R13-H1 (2026-08-22)

R13-H1 (analysis/generation/oracle only — no production change, no commit)
verified every gate above. Full proof in
`docs/R13H1_BATTLE_SCRIPT_GRAPH_REPORT.md`; generated artifacts under
`resources/extraction/emerald/bpee01/battle/` (all `--check` byte-identical).

**Baseline corrections (§1.1, §18).** The plan's byte figures were
address-gap widths including inter-object `.balign` padding; the qualified
`sh_size` payloads are: battle_scripts_1 13,099 (−1), battle_scripts_2 493
(−3), anim 63,811 (−1), AI 9,303 (0), contest 2,524 (0), field-effect 817
(−3) — **90,047 B total** (plan 90,055; the −8 is padding only). Reloc
counts 1,562/4,231/1,222/364/170 are exact. Span-based module count is
**2,089** (plan ≈2,056), so the §18 pack estimate becomes 20,988 + 2,089 =
**23,077** of 32,768 (plan's ≈23,049 was on the old module estimate).

**Classification corrections (§1.2).** SCRIPT_TARGET ≈3,728 → **3,798**;
EWRAM ≈480 → **488** (23 unique symbols); anim CALLBACK 962 → **963** (213
unique; 280 incl. FE); FE gfx ≈40 → **36** / callnative ≈63 → **67** (103
total exact); battle "≈14 residual" → **0** (battle row sums exactly);
anim "6 global + 3 misc" → **8 gIceCrystalSpiralInward{Small,Large} table
targets + 1 callback**. 65 stored-zero u32 slots are NULL sentinels
(tryfaintmon 31, playanimation 30, playanimation_var 2, jumpifword 1, bs2
1), not unresolved relocs.

**All §1/§2/§18 pins now exact:** grammars 249/48/99/136/8 opcodes vs the C
tables; closure universe=included=7,549 with excluded/remainder/unresolved/
ambiguous/unknown all 0; 718 engine bindings (no anonymous addresses);
3,058 nonzero addends all classified (2,778 section-relative targets, 280
EWRAM byte/u16/u32 fields, 27 unique pairs) with bounds all OK; interior
targets 0 (MoveEnd 137 / ButItFailed 95 fan-in confirmed); cross-family
edges 0 (H4/H5/H6 provably independent); 13 routing tables / 768 rows
(764 ptr + 4 u16); 9 State-v5 surfaces / 40 slots (worst-case 27 records
of 4,096); 5 family arenas → 6,382 of 8,192 live ranges; oracle 7,549
checked / 0 bad.

**§17/§19 confirmed:** H1–H7 subdivision stands as written; infra reuse B
(generator) + C (runtime) stands; H3's nested interior-IP STOP gate remains
the gate to H4.

## 27. Confirmed by R13-H3 (2026-08-22)

R13-H3 (State-v5 execution readiness / shadow relocation only — no live
cutover, no commit) proved the §23 STOP gate. Full proof in
`docs/R13H3_BATTLE_STATE_READINESS_REPORT.md`; the shadow seam
(`emerald_battle_compat.c` + generated `battle_native_table.*`) is
harness-linked only through H3; production links the weak
`emerald_battle_state.c` adapter whose no-generation fallthrough keeps
the compiled-script path byte-for-byte unchanged.

**State surface truth (§14 correction).** 36 H-typed slots + 9
engine-typed slots (census pinned against the exact production
structures). The maximum simultaneously active H sidecar count is
**20** (1 IP + 8 battle stack + 4 selection + 4 palace + 2 anim + 1
stale AI) — the plan's ≈27 estimate was conservative; no correction
upward was needed (cap 4,096, headroom ≈200×). The battle-AI grammar
has no return-valid call sites (all five `call` commands are tail
calls — the +5 successor is never an instruction start), so the AI
test/debug frames use ordinary instruction starts (the identical
NEXT_INSTRUCTION predicate).

**Capture policies (brief sec 13-15).** Field effect: no persistent
instruction pointer exists (function-local cursor,
field_effect.c:705) — an FE-arena pointer in any serialized field is a
precise policy refusal. Battle AI: synchronous run-to-completion —
relocate the quiescent stale `gAIScriptPtr`, never execute it; the AI
stack is size-0 at every VBlank. Contest AI: the shared IP slot accepts
either AI family; contest stack relocation is proven for mid-contest
captures.

**Identity model (brief sec 21-23).** Two physical layouts prove
module identity is never an aggregate offset: layout 0 (GBA-preserving,
h2_stage geometry) and layout 1 (tight-packed modules, reversed arena
order). The zero-width alias rule: state identity always canonicalizes
to the payload owner (the module whose export is `offset-zero`);
alias keys refuse boundary queries and state resolution; no zero-length
ranges, deterministic export lookup. The five-family range dry run
registers/unregisters by exact family identity on a scratch index clone
(6,377 → 6,382 → 6,377 with unrelated ranges byte-identical).

**Boundary roles (brief sec 17-18).** INSTRUCTION_START and
NEXT_INSTRUCTION both require an exact bytecode-map instruction start;
ENTRYPOINT requires a generated export. Runtime return addresses are
interior instruction boundaries by construction (IP+5 pushes), fully
supported — H1's interior RELOCATION count of 0 constrains relocation
targets, not runtime returns. Middle-of-operand, holes, gaps, past-end,
data/routing spans, aliases, and wrong-family keys all refuse precisely.

**Production isolation.** `git diff` over every H interpreter TU is
empty; the pack stays 23,069 entries (SHA-256
`b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb`);
live ranges stay 6,377 with 0 H ranges; forced release build
23,320,512 B (+13,032 B: the weak adapter + walker hooks) and the
DINFO build (see report) both verify-game-data
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`; release carries zero debug
sections; `nm` finds no `EmeraldBattleCompat` symbol in either binary.
