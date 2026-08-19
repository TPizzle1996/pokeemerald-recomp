# R13C-AC-1 — Text inventory audit correction (re-pin of §1–§4)

Status: **PROVEN — recorded before any generator work** (brief §1 gate).
Date: 2026-08-18. Method: recomp-source enumeration × qualified ELF
(`../pokeemerald-reference/pokeemerald.elf`, BPEE01 Rev 0, SHA-1
f3ae088181bf583e55daf962a92bb46f4f1d07b7) × ROM byte scan (canonical =
bytes to first `0xFF` from label start, 4 KiB bound) × Gen3 charmap
validation (`/tmp/r13c_charmap.py`; 0 malformed). The plan's own §1 note
declares recomputed figures authoritative where they differ from the R13-A
audit; this document is that correction, per the brief's §1 discrepancy
gate.

## 1. Headline result

| Pin | Plan | Re-pinned | Delta | Verdict |
|---|---|---|---|---|
| Text labels (total) | 12,756 | **12,756** | 0 | **EXACT** |
| Canonical bytes (total) | 901,781 | **900,887** | −894 (−0.10%) | corrected, proven |
| Script-side labels (script_data) | 7,913 | **7,913** | 0 | **EXACT** |
| Script-side bytes | 747,007 | **747,007** | 0 | **EXACT** |
| C-side labels (.rodata) | 4,823 | 4,843 | +20 | composition, see §3 |
| C-side bytes | 154,774 | 153,880 | −894 | corrected, proven |
| Empty strings (1 B) | 19 | **19** | 0 | **EXACT** |
| Single-char (2 B) | 50 | **50** | 0 | **EXACT** |
| Duplicate-byte groups | 476 | **476** | 0 | **EXACT** |
| Duplicate-group symbols | 1,161 | **1,161** | 0 | **EXACT** |
| Same-address aliases | — | 0 | — | none |
| Redundant duplicate bytes | — | 23,733 | — | (recorded) |
| Script bundles | 362 | **362** | 0 | **EXACT** |
| Dual-ref (script+C) symbols | 11 | **97** | +86 | corrected, proven |
| min/median/max label bytes | 1/49/824 | **1/49/824** | 0 | **EXACT** |

Size distribution confirms the plan's 4 KiB scan bound was never hit (max
824 B) — every canonical size is exact-by-scan, not bounded.

## 2. Family-table comparison (25 plan families)

22 of 25 families match **exactly** in labels AND bytes:
map-dialogue 4,357/427,829; trainer 1,142/67,095; match-call 629/60,934;
pokedex 387/58,017; tv 386/53,635; apprentice 288/49,891; misc-scripts
511/37,399; shared 1,655/29,909; move 355/17,251; item 310/15,101; frontier
180/11,597; cable-club 91/7,713; easy-chat 1,007/7,095; berry 41/4,305;
pokemon-news 12/3,950; ability 78/1,859; pokedex-rating 25/1,780; ribbon
66/1,265; frontier-brain 28/882; mauville-man 18/638; nature 25/162; mart
3/87.

Three rows carry documented deltas (all proven, none STOP-triggering):

### 2a. system: 615/28,865 — EXACT by plan's own decomposition
Plan composition: strings.c non-gText_ 106 + asm-misc 509 (534 − 25
event_scripts.s). Measured: strings.c non-gText **106 / 1,233 B** (exact);
asm-misc 534 / 29,271 B, of which event_scripts.s body = 25 / 1,639 B and
"rest" = 509 / 27,632 B. 106 + 509 = **615 labels ✓**, 1,233 + 27,632 =
**28,865 B ✓**. The plan's system row reproduces exactly under its own
composition rule.

### 2b. misc: 27/2,131 — labels EXACT, bytes −417 (plan sizing artifact)
Plan: 25 event_scripts.s + 2 (gTest_MissedTurn, gText_LinkTVProgramWillNot
BeMadeTrainerLost). Measured: 25 / 1,639 B + 2 / 75 B (12 + 63) = **27
labels ✓**; bytes 1,714 vs plan 2,131 → Δ −417 B. The plan's misc byte row
includes a 417 B sizing artifact — the two leftover labels measure 75 B
total in the qualified ROM; no other label is in that row.

### 2c. battle: 520 → 522 labels; bytes 12,391 → 11,989 (Δ −402)
Measured composition: 519 direct-resolved labels / 11,908 B, plus **3
rename-resolved** (see §4): gText_Judgment 19 B, sText_GotchaPkmnCaught
30 B, sText_GotchaPkmnCaught2 32 B → **522 / 11,989 B**. `sText_EmptyStatus`
(recomp-only `_("$$$$")`, 8 source B) is excluded: the byte pattern
`0x24 0x24 0x24 0x24 0x24 0x24 0x24 0xFF` is **absent from the qualified
ROM** (whole-ROM scan; no symbol at any plausible address yields it) — the
recomp's settings-UI-only label has no ROM bytes and stays compiled out of
contract. The plan's battle byte row (12,391) exceeds the measured 11,989
by 402 B; a scan of the entire ELF symbol table confirms **no text label of
~483 B exists** in the qualified ROM (max battle label 225 B), so the
plan's battle byte figure is a design-phase sizing artifact, not a content
difference. Label count 520 → 522: the plan's 520 counted the direct-519
plus gText_Judgment only; the gotcha pair (CaughtPlayer/Wally → Caught/
Caught2, both ROM-resolvable via rename) complete the row.

## 3. Section split and total reconciliation

```
measured total 12,758  = 7,913 script_data + 4,845 .rodata
  .rodata 4,845       = 4,820 plan-scope C labels + 20 gift + 3 renames + 2 c-misc
  c-misc 2 (22 B)     = gSpeciesNames + gTrainerClassNames (structured 2D name
                        tables, NOT text strings — excluded from migration)
measured total 12,756  = 7,913 + 4,843   (gift included, c-misc excluded)
plan total    12,756  ✓ EXACT
```

The plan's own bookkeeping (documented in the plan): section rows sum to
12,736, total 12,756 — the 20 gift labels resolve in `.rodata` via their C
defs but count in script-side by source. Re-pin reproduces that exactly:
plan-scope C 4,820 + gift 20 = 4,840 `.rodata` records; + 3 renames =
4,843. The plan's .rodata row (4,823) = 4,820 + 3 renames (gift excluded
from that row). Both bookkeepings equal 12,756 total. **No STOP condition:
the label pin is exact.**

Bytes: 900,887 = 747,007 (script, exact) + 153,880 (rodata). The −894 B
vs plan decomposes as battle −402 (§2c) + misc −417 (§2b) + 75 B residual
in plan-internal rodata rows (the 3 rename labels' 81 B − 6 B double-count
in the plan's battle row). All deltas are sizing artifacts of plan rows
that measured byte-exact under the plan's own composition rules wherever
those rules are fully specified; the two irreproducible rows (battle, misc)
are quantified with negative evidence (phantom sizes absent from the ELF).
**STOP condition "canonical total differs from 901,781 B" — this
correction is the required proven audit correction and the work proceeds
under the re-pinned 900,887 B.**

## 4. Rename map (recomp source → qualified reference ELF)

The recomp's vendored source is a **newer pret revision** than the
reference tree. Label renames required at generation time:

| Recomp source label | Reference ELF label | Canonical B | In migration |
|---|---|---|---|
| gText_Judgment | gText_Judgement | 19 | yes (battle) |
| sText_GotchaPkmnCaughtPlayer | sText_GotchaPkmnCaught | 30 | yes (battle) |
| sText_GotchaPkmnCaughtWally | sText_GotchaPkmnCaught2 | 32 | yes (battle) |
| sText_EmptyStatus | — (no ROM bytes) | — | **no** (stays compiled) |
| 12 settings-UI labels (gText_VSync, gText_Volume, gText_WindowScale, gText_Back, gText_BorderBackground, gText_BorderBackgroundName, gText_BorderBackgroundOff, gText_BorderFrame, gText_DisplaySettings, gText_Fullscreen, gText_IntegerScale, gText_PkmnFainted_FldPsn) | — (no ROM bytes) | — | **no** (out of contract) |
| 20 asm labels (bundle-local renames, e.g. event_scripts.s body) | renamed in recomp asm | — | yes (bundle-local, no impact) |

The 12 settings-UI strings are recomp-only additions (PC recomp options UI)
with no retail ROM presence — they stay compiled, out of the migration
contract, matching the plan's §1 note on recomp-only labels.

## 5. Dual-reference count: 11 → 97 (proven)

The plan's §7 list ("e.g. ...") named a partial enumeration. A full sweep
of every `src/*.c` (comment-stripped, line-classified: bare `L,` /
`[expr] = L,` / `{ ... },` rows = array entries → NOT dual-ref; any other
use = direct code ref; `extern` declarations excluded) over the 7,913
script-side labels yields **97 asm labels with direct C code references**
(full list archived with this run; families: asm-misc 62, pokedex-rating
25, misc-scripts 14, map-dialogue 6, cable-club 4, mauville-man 2 —
gBirchDexRatingText_* (25), contest.c text (≈35), roulette (13), secret
base (10), save/start_menu (8), cable-club (4), Birch intro (8), etc.).
All 97 are in **deferred** families — none are in the R13-C live cutover
families (battle/move/ability/nature/contest) — so the cutover mechanics
are unchanged; the exemption set (stays COMPILED_PENDING_MIGRATION, slot
conversion deferred to R13-G with script-operand re-emission) grows from
11 to 97, each individually documented in the generator's ownership
artifacts. Table-row references (sMatchCallTrainers 312, sTV*TextGroup,
gBattleStringsTable rows, sContestPaintingDescriptions, sNatureGirl...,
battle_dome/tent tables) are NOT dual-refs — they regenerate as skeleton
tables per §10.

## 6. STOP-condition audit (brief §25)

| STOP condition | Status |
|---|---|
| Text label count ≠ 12,756 | **EXACT — no trigger** |
| Canonical total ≠ 901,781 B | corrected per this document (900,887 B, Δ −894 proven) |
| Resource count ≠ 5,185 | not yet measured (generator stage) |
| Canonical encoding requires normalization | no — charmap grammar 0/12,756 malformed |
| Script operands must be rewritten | no (additive deferral unchanged) |
| One-range-per-label | no — per-family arenas unchanged |
| Range cap raise | no — 16 family ranges within 8,192 |
| Silent compiled fallback in live families | no — not at cutover stage yet |
| TextPrinter.currentChar relocation | untouched (R13 §13 stage) |
| Pointer-slot conversion call-site edits | zero — slot mechanism unchanged |
| Field-script VM changes | none |
| R12/R13-B regressions | no code changed yet (re-pin only) |

## 7. Working pins (authoritative for the generator, §5–§7)

**Final corrected values (AC-2 §8 + AC-3 §9; the generator gates on
these):**

- **12,777 labels / 903,151 canonical B** (battle 522 incl. 3 renames;
  +21 rename-completed labels per AC-3 §9a; EmptyStatus + 12 settings-UI
  stay compiled out of contract)
- **Script-side: 7,933 / 749,265 B** (7,913/747,007 + 20 renames)
- **C-side plan-scope: 4,821 / 151,123 B** (4,820/151,117 + WeRe 6 B;
  excludes gift 20/2,682, renames 3/81, c-misc 2/22)
- **Resources: 4,824 C-side per-label + 363 bundles + 5,187 total**
- **Duplicates: 478 groups / 1,168 symbols / 23,891 redundant B — kept
  (no dedup) per §4**
- **Dual-ref: 97** (documented; all deferred families)
- **Bundles: 363 = 304 maps + 35 data/text + 24 data/scripts; pack
  6,876 → 12,063 entries**

## 8. AC-2 — Bundle count 362 → 363 (proven)

Discovered during generator bring-up (2026-08-18), before any artifact was
emitted. The plan's 362 = 304 + 35 + 23 and AC-1 §7's "EXACT" were both
computed from an enumeration that tallied only `data/maps/`,
`data/text/`, and `data/scripts/` prefixes. That enumeration leaves
**`data/event_scripts.s`** (25 labels / 1,639 B — the plan-misc family,
inside the 7,933-asm pin and the 900,887-B total, both verified EXACT)
in **no bucket**. A full per-file probe (all 363 files enumerated, label
counts per file, `data/event_scripts.s: 25`) shows:

```
files with asm text labels: 363 = 304 maps + 35 data/text + 23 data/scripts
                              + 1 data/event_scripts.s
```

Every asm label is bundle-local by the plan's own model (§2), so the
event_scripts.s labels need a bundle: `emerald:text/scripts/event-scripts`
(plan vocabulary `emerald:text/scripts/<file>`). Consequences:

| Pin | Plan/AC-1 | Corrected | Δ | Proof |
|---|---|---|---|---|
| Bundles | 362 | **363** | +1 | per-file probe; the 25 labels are in every total pin (7,933 asm / 900,887 B) |
| Data/scripts bundle bucket | 23 | **24** | +1 | event_scripts.s resolves at `data/event_scripts.s`, not under `data/scripts/` |
| Total text resources | 5,185 | **5,186** | +1 | 4,823 + 363 |
| Production pack entries | 12,061 | **12,062** | +1 | 6,876 + 5,186 |

No label or byte pin moves (12,756 / 900,887 / 7,913 / 747,007 / 4,823 /
151,198 / 7,933 / 749,689 unchanged — the correction touches only the
bundle decomposition). STOP-condition check: "resource count ≠ 5,185" is
met by this proven correction, per the same clause AC-1 used for the byte
total. The plan's §13 unit (ROM-hack import) is unchanged — event_scripts.s
is its own unit either way.

## 9. AC-3 — Rename map completion (21 labels) + system double-count (proven)

Discovered during generator bring-up (2026-08-18). Two independent
findings, both measured from the qualified ELF × ROM with the generator's
own resolver (every byte a ROM slice under a content-verified reference
label).

### 9a. Twenty-one labels missing from the inventory via renames

AC-1's inventory applied the rename map for only the 3 battle labels.
The generator's fail-closed unresolved gate (must equal exactly the 13
out-of-contract labels) surfaced **21 further source labels whose bytes
ARE in the qualified ROM under reference-tree names** — the newer pret
revision renamed them; AC-1's measurement dropped them silently
(`name not in elf → continue`):

| Recomp source label | Reference ELF label | B | Family |
|---|---|---|---|
| BattleFrontier_BattleTowerLobby_Text_DirectYouToBattleRoom | …_EventScript_DirectYouToBattleRoom | 41 | map-dialogue |
| OldaleTown_Text_TownSign | OldaleTown_Text_CitySign | 45 | map-dialogue |
| Route110_TrickHousePuzzle7_Text_WroteSecretCodeLockOpened | …_EventScript_WroteSecretCodeLockOpened | 110 | map-dialogue |
| Route110_TrickHousePuzzle8_Text_WroteSecretCodeLockOpened | …_EventScript_WroteSecretCodeLockOpened | 105 | map-dialogue |
| MauvilleCity_PokemonCenter_1F_Text_HaveYouHeardOfWord | …_Text_HaveYouHeardOfPhrase | 85 | misc-scripts |
| gEasyChatWord_WeRe | gEasyChatWord_WeAre | 6 | easy-chat |
| BravoTrainerBattleTower_Text_Intro/NewRecord/Lost/Won/LostFinal/Satisfied/Unsatisfied/None1-4/Response/ResponseSatisfied/ResponseUnsatisfied/Outro (15) | gTVBravoTrainerBattleTowerText00–14 | 1,872 | tv |

Each pair is content-verified: 11 text-matched (`\n`/`\p`/`\l`-normalized
.string payloads identical), 4 order-matched ("None$" ×4, source order
identical in both revisions → Text07–10), 5 single-label matches by
distinctive text fragment. All 21 reference targets resolve in the ROM as
text labels (charmap-valid, script_data/.rodata as expected).

### 9b. System family double-count (plan + AC-1 arithmetic)

AC-1 §2a's decomposition `system = 106 + 509` counted the 2
contest_link.inc labels (`gTest_MissedTurn` 12 B, `gText_LinkTVProgram
WillNotBeMadeTrainerLost` 63 B) inside "asm-misc rest 509" while §2b also
counts them in misc (27 = 25 + 2). The family rows therefore double-count
2 labels / 75 B across system and misc; the row sum exceeds the (exact)
total by 2. Measured system = **613 / 28,790 B** (106 strings.c
non-gText_ 1,233 B + 507 asm-misc rest 27,557 B; 1,233 + 27,557 =
28,790 ✓). misc = 27 / 1,714 B unchanged and exact.

### 9c. Corrected pins (all measured by the generator's resolver)

| Pin | AC-1/plan | Corrected | Δ |
|---|---|---|---|
| Total labels | 12,756 | **12,777** | +21 |
| Total canonical bytes | 900,887 | **903,151** | +2,264 |
| Script-side (script_data) | 7,913 / 747,007 | **7,933 / 749,265** | +20 / +2,258 |
| C-side resources | 4,823 / 151,198 | **4,824 / 151,204** | +1 / +6 |
| Asm-source (incl. gift) | 7,933 / 749,689 | **7,953 / 751,947** | +20 / +2,258 |
| Gift (asm→.rodata) | 20 / 2,682 | 20 / 2,682 | — |
| Family rows | 22/25 exact | 24/25 exact | system row corrected |
| system | 615 / 28,865 | **613 / 28,790** | −2 / −75 |
| map-dialogue | 4,357 / 427,829 | **4,361 / 428,130** | +4 / +301 |
| misc-scripts | 511 / 37,399 | **512 / 37,484** | +1 / +85 |
| tv | 386 / 53,635 | **401 / 55,507** | +15 / +1,872 |
| easy-chat | 1,007 / 7,095 | **1,008 / 7,101** | +1 / +6 |
| Duplicate-byte groups | 476 / 1,161 / 23,733 | **478 / 1,168 / 23,891** | +2 / +7 / +158 |
| Empty / single-char | 19 / 50 | 19 / 50 | — |
| min/med/max | 1/49/824 | 1/49/824 | — |
| Dual-ref | 97 | 97 (gate re-proves) | — |
| Out-of-contract | 13 | 13 (unchanged) | — |
| Total resources | 5,186 | **5,187** | +1 (4,824 + 363) |
| Production pack entries | 12,062 | **12,063** | +1 |

Byte cross-check: 903,151 = 749,265 + 153,886 (.rodata) ✓;
153,886 = 151,204 + 2,682 (gift) ✓; the 21 labels' 2,264 B decompose as
301 + 85 + 1,872 + 6 ✓. STOP-condition check: "text label count ≠ 12,756"
and "canonical total ≠ 901,781 B" are met by this proven correction under
the same clause as AC-1 (both totals now measured, not estimated). The
97-dual-ref and 13-out-of-contract pins are re-proven by the generator's
fail-closed gates on every run.
## 10. AC-4 — Dual-ref classifier bug + TV array count (proven)

Discovered during generator bring-up (2026-08-19), before any artifact was
emitted. Two generator-side measurement bugs, both fixed with proofs.

**10a. Dual-ref sweep line-merge bug (135 vs 97).** The generator's first
`find_dual_refs` stripped `//` comments by deleting everything from `//` to
*and including* the newline, merging two source rows into one line
(`BattleDome_Text_X,  // comment` + `BattleDome_Text_Y,` became a single
two-label line). The row classifier then failed the bare-`L,` match and
flagged 38 extra labels as code references (28 battle_dome, 4 tv, 4
system/field_specials/battle_message, 2 battle_dome table rows already
counted — total 135 vs the AC-1 §5 proven 97). Fix: `strip_comments` now
preserves newlines, and `find_dual_refs` implements the documented AC-1 §5
line-kind classifier verbatim (`L,` / `[expr] = L,` / `{ ... },` rows and
extern/typedef lines are not references). Re-measurement: exactly **97**,
matching the pin; all 38 false positives verified as array rows by
inspection (battle_dome.c:1306-1343 Emphasizes/Neglects rows,
tv.c sTV*TextGroup rows, field_specials.c:2457-2471, battle_message.c:
880-881). No pin change — the 97 gate re-proves.

**10b. sTV*TextGroup count 31 → 32.** The generator's
`SKELETON_TV_ARRAY_COUNT = 31` was a bring-up miscount; the "31" appears
nowhere in the plan or AC-1 (the R13-A audit even said 29 — approximate).
Measurement: **32 arrays in BOTH the qualified reference tree and the
recomp tree** (`grep -c 'static const u8 \*const sTV\w*TextGroup\['` =
32 in each; the reference has `sTVPokemonAnslerTextGroup`, the recomp has
the typo-fixed `sTVPokemonAnglerTextGroup`, otherwise identical). Pinned
constant corrected to 32; the skeleton-table count is now 53 = 7 pinned +
32 TV + 14 match-call.

**10c. Collision-group artifact filenames.** The WeRe/Were pair
(AC-3 §9a) shares the canonical name `geasychatword-were`; per-label
artifact paths derived from the canonical name collided and the second
write overwrote the first (4,823 artifacts instead of 4,824; `--check`
failed on the clobbered file). Fix: artifact filenames use the resource
key's last segment (`geasychatword-were.bin` / `geasychatword-were-1.bin`),
which is the canonical name for every other label. Re-measurement: 4,824
artifacts, deterministic `--check` no-op, exit 0.

**AC-4 status: no working-pin changes** (all AC-1/AC-2/AC-3 pins re-prove
unchanged: 12,777/903,151; 4,824+363; dups 478/1,168/23,891; 19/50;
dual-ref 97; OOC 13; bundles 363).
