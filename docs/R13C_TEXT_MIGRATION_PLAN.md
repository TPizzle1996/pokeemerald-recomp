# R13-C — Emerald Text/String Ownership Migration Plan

Scope: design review only. This document settles resource granularity,
identity, publication, consumer cutover, State-v5 impact, and isolation for
the R13-C text migration. **No implementation. No commit. R13-D not started.**

Grounding: `docs/R13_REMAINING_CONTENT_AUDIT.md`,
`docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md`,
`docs/R13B_LEAF_PAYLOAD_MIGRATION_REPORT.md`,
`docs/EMERALD_ROM_BACKED_ASSET_MIGRATION_ARCHITECTURE.md`,
`docs/R10_NATIVE_STATE_V5_DESIGN.md`, `docs/R12G_AUDIO_ISOLATION_REPORT.md`.

All inventory figures below were **recomputed from scratch** against the
qualified pret reference build (`../pokeemerald-reference/pokeemerald.elf`
+ `pokeemerald.gba`, BPEE01 Rev 0, SHA-1
f3ae088181bf583e55daf962a92bb46f4f1d07b7) with a reproducible method:
(1) enumerate every source-defined text label (`data/**/*.inc`, `data/*.s`,
`src/strings.c`, `src/battle_message.c`, `src/data/text/*.h`,
`src/data/pokemon/*.h`, `src/data/easy_chat/*.h`); (2) resolve each in the
qualified ELF symbol table; (3) size each canonical string by the engine's
own semantics (scan to the first `0xFF` terminator, bounded at 4 KiB); (4)
validate every payload against the engine escape grammar
(`include/constants/characters.h`: 00–F6 single chars, F7 dynamic,
F8/F9/FC/FD two-byte escapes, FA/FB/FE singles, FF terminator). 0 labels
failed validation. Where this plan's figures differ from the R13-A audit
(which was approximate on the C side), the recomputed figure is authoritative
and the delta is called out.

## 1. Exact inventory (pinned)

| Quantity | Value |
|---|---|
| Distinct source-defined text labels | **12,756** |
| Canonical text bytes (incl. every `0xFF` terminator) | **901,781 B** |
| String size min / median / max | **1 / 49 / 824 B** (payload incl. terminator) |
| Empty strings (1 B, terminator only) | 19 |
| Single-char strings (2 B) | 50 |
| Section placement | script_data 7,913 labels / 747,007 B; `.rodata` 4,823 labels / 154,774 B |
| Same-address aliases (multiple labels, one address) | **0** (1 pseudo-alias: `$d` assembler data marker vs `gText_ExpandedPlaceholder_Empty`) |
| Byte-identical duplicates (same bytes, distinct addresses) | **476 groups**, 1,161 symbols have ≥1 byte-twin, 23,733 B redundant (2.6% of payload) |
| Symbols referenced by both C code and script operands | **11** |

### 1.1 By family (labels / canonical bytes)

| Family | Labels | Bytes | Referee shape |
|---|---|---|---|
| Map dialogue (`data/maps/*/scripts.inc`, 264 main + 40 BattleFrontier) | 4,357 | 427,829 | script operands (intra-object locals) |
| Trainer text (`data/text/trainers.inc`) | 1,142 | 67,095 | script operands |
| Match call (317 `.inc` + 312 C) | 629 | 60,934 | script + direct C (624 refs) |
| Pokédex entries (`pokedex_text.h`) | 387 | 58,017 | `gPokedexEntries[]` rows + direct C (387 refs) |
| TV (386 `.inc` labels + `sTV*TextGroup` tables) | 386 | 53,635 | script + direct C (321 refs) |
| Apprentice | 288 | 49,891 | direct C (288 refs) |
| Misc scripts text (`data/scripts/*.inc`, 23 files) | 511 | 37,399 | script + some direct C (120) |
| Shared pool (`gText_*` in `src/strings.c`) | 1,655 | 29,909 | direct C (1,529 refs) |
| System (`sText_*`, `Text_*`, misc system strings) | 615 | 28,865 | direct C (366) + script (42) |
| Move descriptions | 355 | 17,251 | `gMoveDescriptionPointers` only — **0 direct refs** |
| Item descriptions | 310 | 15,101 | `gItems[].description` rows only — **0 direct refs** |
| Battle strings (`sText_*`, battle_message.c) | 520 | 12,391 | `gBattleStringsTable` (ID-indexed); 50 shared `gText_*` also direct-ref'd |
| Frontier (battle_dome/battle_tent) | 180 | 11,597 | script + direct (114) |
| Cable club | 91 | 7,713 | script + direct (12) |
| Easy-chat words (`gEasyChatWord_*`) | 1,007 | 7,095 | compiled into `sEasyChatGroupWords` blobs (1,007 refs) |
| Berry names (`data/text/berries.inc`) | 41 | 4,305 | `gBerries[]` rows |
| Pokémon news | 12 | 3,950 | direct C (12) |
| Misc (TV/fan-club leftovers, `Text_*` boot/pc) | 27 | 2,131 | mixed |
| Ability descriptions | 78 | 1,859 | `gAbilityDescriptionPointers` only — **0 direct refs** |
| Pokédex rating | 25 | 1,780 | direct C (24) + script (4) |
| Ribbon descriptions (incl. gift ribbons) | 66 | 1,265 | direct C (66) |
| Frontier brains | 28 | 882 | direct C (28) |
| Mauville man | 18 | 638 | direct C (18) |
| Nature names | 25 | 162 | `gNatureNamePointers` only — **0 direct refs** |
| Mart clerk | 3 | 87 | script |

Delta vs the R13-A audit's ≈874,022 B: the audit's "tv 25,160 B" covered
only part of `tv.inc` (this plan measures the whole file: 386 labels /
53,635 B); the audit also lumped row-table text (trainer-class/ability/type
inline names, Pokédex category names — 5,028+ B of **non-string row tables,
excluded here**) into its C-side total. Both are bookkeeping differences, not
payload disagreements: this plan's per-label measurement is the migration
contract.

**Excluded from R13-C by design** (not string resources):
- `gSpeciesNames` / `gMoveNames` — host-hydrated from the legacy content
  package; absorbed in R13-D, not R13-C.
- Inline row tables `gTrainerClassNames` / `gAbilityNames` / `gTypeNames` —
  fixed-width padded rows, no `0xFF` terminators; they migrate with their
  owning structured tables (R13-E).
- `sPokedexCategoryName_*` — struct-embedded inline fields of
  `gPokedexEntries` (R13-E).
- `sEasyChatGroupWords` concatenated word blobs — R13-B-remaining leaf
  family, not string resources.
- The 9 unlinked `data/scripts/gift_*.inc` mystery-gift texts (absent from
  the qualified ELF, per the R13-A audit).

### 1.2 Dedup policy input

476 duplicate-content groups exist (e.g. 19 copies of the empty string; 11
copies of "CANCEL"; 9 identical Battle-Tent lobby headings). **No pack-level
deduplication.** Each label stays its own canonical resource: a mod
overriding one copy must not silently change its twin (the `sBerryTree_Text_`
/ `sText_ExclamationMark`-style aliasing is deliberately preserved in the
vanilla ROM and mods may rely on the split). Duplicate groups are emitted as
generator metadata (catalog side table) for mod tooling only.

## 2. Resource identity granularity — hybrid (option D)

| Scope | Identity | Count | Rationale |
|---|---|---|---|
| Script-side text (`data/maps`, `data/text`, `data/scripts`) | **per-map / per-file bundle** | **362 bundles** (304 maps + 35 data/text + 23 data/scripts) covering 7,933 labels | script replacement is per-map/per-file anyway (R13-G); a bundle preserves every intra-file label offset and matches the ROM-hack unit (§13) |
| C-side text (all `.rodata` families) | **per-label** | **4,823 resources** across 29 files | the mod surface for one-item/one-move/one-string overrides; labels are stable pret symbols |
| **Total new text resources** | | **5,185** | pack 6,876 → **12,061 entries** |

Per-label identity for 4,823 C-side labels is the right granularity: these
names (`gText_Cancel`, `sPoundDescription`, `gBulbasaurPokedexText`) are
stable semantic labels — there is no 12,000-opaque-key problem. Script-side
labels are NOT individually resource-keyed (they are bundle-local; per-label
identity would add ~7,900 entries with no mod value beyond the bundle
override).

Key vocabulary (`text` type, schema 1, representation `gba-charmap`):

```
emerald:text/map/<MapName>                 e.g. emerald:text/map/PetalburgCity
emerald:text/data/<file>                   e.g. emerald:text/data/trainers
emerald:text/scripts/<file>                e.g. emerald:text/scripts/mauville_man
emerald:text/item/<ITEM_ID>                e.g. emerald:text/item/master-ball
emerald:text/move/<MOVE_ID>                e.g. emerald:text/move/pound
emerald:text/ability/<ABILITY_ID>          e.g. emerald:text/ability/stench
emerald:text/nature/<NATURE_ID>            e.g. emerald:text/nature/hardy
emerald:text/pokedex/<SPECIES>             e.g. emerald:text/pokedex/bulbasaur
emerald:text/battle/<symbol>               e.g. emerald:text/battle/sText_Win
emerald:text/system/<symbol>               e.g. emerald:text/system/gText_Cancel
emerald:text/tv/<symbol>                   emerald:text/matchcall/<symbol>
emerald:text/apprentice/<symbol>           emerald:text/ribbon/<symbol>
emerald:text/frontier/<symbol>             emerald:text/frontier-brain/<symbol>
emerald:text/easychat/<symbol>             emerald:text/berry/<symbol>
emerald:text/pokedex-rating/<symbol>       emerald:text/pokemon-news/<symbol>
emerald:text/mauville-man/<symbol>         emerald:text/mart/<symbol>
```

ID-mapped keys (item/move/ability/nature/pokedex) are derived from the
generator's enum-position bindings (the same declarative lists that build the
GBA tables, per the architecture doc's X-macro rule); symbol-form keys are
the pret label verbatim. The M0/M1 rule
`SHA-256("gen3-resource-id-v1\0" + exact canonical-name bytes)` applies as
always. Bundle-local labels are published in the generator's bindings as
`<bundle> :: <label>` provenance, never as resource keys.

## 3. Alias policy

Measured: **0 same-address aliases** (one `$d` pseudo-alias, not a label).
The alias surface is therefore entirely **byte-duplicate twins** (476
groups) and **cross-object double references** (11 symbols).

Policy:

1. **One resource per source label.** Labels are never merged at the pack
   level, even when byte-identical (a mod override must be scoped to exactly
   one label). Duplicate groups are catalog metadata only.
2. **`legacySymbol` bindings** (the R13-B column) carry every label name per
   resource — including the 11 double-referenced symbols and bundle-local
   labels. Alias identity for isolation = symbol set equality against the
   generated ownership table, never name-prefix guessing.
3. **Duplicate-byte resources stay separate ROM slices** — each carries its
   own romOffset/size/hashes; the disjointness proof (R13-B E8 pattern)
   permits distinct ranges that happen to contain equal bytes.
4. **Empty/minimal strings are ordinary resources** (19 × 1 B, 50 × 2 B).
   No special-casing in the pack; only the isolation byte-scan has a
   documented length floor (see §12).

## 4. Canonical representation

Unchanged from the architecture contract: **exact Gen III charmap bytes,
`0xFF`-terminated, control codes and placeholders untouched.** No UTF-8
conversion, no normalization, no decode/re-encode in the pack. A decoded
form (for mod tooling) is generator metadata only.

Per-resource chain, machine-checked by the generator (`--check`, the
R13-B three-way pattern + the escape-grammar validator):

```
source label (inc/h) → qualified ELF bytes == ROM slice
                     → escape-grammar valid (0 failures of 12,756)
                     → payload == pack payload == arena bytes
```

The qualified ELF/ROM pair pins `romOffset` + `romSha1` provenance per
record exactly as R13-B did.

## 5. Pointer-table / live consumer audit

Every consumer family traced; classification per the brief:

| Family | Pointer source today | Reads through `HostResolveGbaAddr`? | Class |
|---|---|---|---|
| Field-script messages (`msgbox`, `showmessage`, `trainerbattle`) | script operands = link-patched **native** addresses | **yes** (`ScriptReadPointer`, scrcmd.c:72) | DEFER_TO_SCRIPT/MAP_STAGE |
| Battle-script text reads (`T1_READ_PTR`/`T2_READ_PTR`) | script operands (native addrs) | **yes** (battle_script_commands.c) | DEFER_TO_SCRIPT/MAP_STAGE |
| Battle message IDs | `gBattleStringsTable[stringID]` in `.rodata` (517 rows) | no — direct table deref (battle_message.c:2243/2778) | LIVE_CAN_CUT_OVER_DIRECTLY (skeleton table) |
| Move descriptions | `gMoveDescriptionPointers[354]` | no | LIVE_CAN_CUT_OVER_DIRECTLY (skeleton table) |
| Ability descriptions | `gAbilityDescriptionPointers[80]` | no | LIVE_CAN_CUT_OVER_DIRECTLY |
| Nature names | `gNatureNamePointers[25]` | no | LIVE_CAN_CUT_OVER_DIRECTLY |
| Contest effect/move-type text | `gContestEffectDescriptionPointers[48]`, `gContestMoveTypeTextPointers[5]` | no | LIVE_CAN_CUT_OVER_DIRECTLY |
| Shared/system strings (`gText_*` direct) | 7,330 direct relocations from 112 code objects (5,547 distinct symbols) | no — `PrintString(gText_X)`-style immediates | NEEDS_SLOT_REEMISSION (see below) |
| Std menu strings | `gStdStrings` | no | NEEDS_SLOT_REEMISSION + skeleton table |
| Item descriptions | `gItems[].description` struct-embedded (`.rodata`) | no | DEFER_TO_TABLE_STAGE (R13-D repoints `gItems`) |
| Pokédex entries + category | `gPokedexEntries[]` rows + 387 direct C refs (pokedex.o) | no | DEFER_TO_TABLE_STAGE (R13-E) |
| Berry names | `gBerries[]` rows | no | DEFER_TO_TABLE_STAGE (R13-E) |
| Easy-chat words | compiled into `sEasyChatGroupWords` blobs | no (blob-indexed `CopyEasyChatWord`) | DEFER_TO_TABLE_STAGE (with the R13-B-remaining leaf-blob family) |
| TV / match-call / apprentice / ribbon / frontier / mauville-man / news / rating | per-family C tables + 1,600+ direct refs | no | NEEDS_SLOT_REEMISSION (+ skeleton tables for `sTV*TextGroup`, `sMatchCallTrainers`) |
| Map/NPC/sign text | script operands (intra-object locals — no relocations) | yes | DEFER_TO_SCRIPT/MAP_STAGE |
| Braille text | script operands (map labels) | yes | DEFER_TO_SCRIPT/MAP_STAGE |

**The slot mechanism (NEEDS_SLOT_REEMISSION).** For the direct-referenced C
side, each text symbol becomes a generated host-`.data` pointer slot with the
same name (`HOST_DATA const u8 *gText_Cancel;`), filled from the arena at
publish; the GBA flavor keeps the original array definition (native-only
conditional, the R12-G `#ifdef NATIVE_LINUX` pattern). Zero call-site edits:
`PrintString(gText_X)` passes the slot's value, which is the arena pointer.
Tables in `.rodata` whose initializers reference slot-converted symbols
(e.g. `gStdStrings`, `sTV*TextGroup`, `sMatchCallTrainers`) are regenerated
as skeleton tables in host `.data` (the R9 `gTrainerFrontPicTable` pattern)
with rows filled at publish. Payload bytes leave the binary (slots hold
pointers only).

**The 11 double-referenced symbols** (script operands + C code, e.g.
`gText_PleaseWaitForLink`, `gText_WhichPCShouldBeAccessed`, 2 BattleFrontier
strings, 3 Birch-rating strings, 4 Lanette's-PC strings) are **exempt from
slot conversion**: they stay compiled arrays through R13-G, documented
per-symbol. Slot conversion would corrupt script reads (the operand would
point at the slot's pointer bytes). After R13-G re-emits script operands as
GBA logical addresses, these 11 migrate with their families.

**Script-side deferral boundary (§6).** Script bytecode operands embed
**native** link-time addresses (the R13-A audit: `R_X86_64_32`, 0 addends;
verified: `call` operand = a native VA). The R12 logical registry is keyed
by GBA addresses, so it cannot serve them without either (a) re-emitting
script operands as GBA addresses — that is R13-G, out of R13-C — or (b)
registering per-label native addresses extracted from the built binary
(impossible for the 7,900+ **local** map/trainer labels: locals are not
linkable, and a build-time nm→generate→relink loop is not acceptable
build-pipeline risk). Therefore: script-side text is **published additively
in R13-C** (arena + pack + provenance) and stays compiled
(COMPILED_PENDING_MIGRATION) exactly like R13-B movement; the compiled copy
serves all live reads; the logical registry (keyed by ROM address, the clean
R12-C pattern) is populated in R13-G alongside operand re-emission with zero
seam changes. No script-graph work enters R13-C.

## 7. Arena / logical mapping design

**No interval mapping. Per-label/per-bundle exact placement in family
arenas.** Rationale, grounded in the measured ROM layout: text is not
contiguous — 7,913 script-side labels are interleaved with script bytecode
inside one 1.06 MB `script_data` section, and C-side text is spread across
`.rodata` with non-text tables between families. Any ROM-interval mapping
would silently alias script bytes into the text arena (the exact R12-F
coarse-zone bug the brief forbids).

Design:

- The text seam allocates **one contiguous arena per family group**
  (script-side per bundle; C-side per family, e.g. all 520 battle strings,
  all 355 move descriptions) and packs payloads **sorted by canonical name**,
  16-byte aligned, byte-exact copies including terminators. Deterministic:
  same inputs → same layout (a runner E0-equivalent asserts byte-identical
  arena layout).
- Script-side live reads keep using compiled bytes in R13-C (no mapping
  needed); the family arena is validated byte-for-byte against ROM slices
  (publication proof), so R13-G can flip the logical registry to it without
  layout changes.
- Cut-over C-side families are published to their family arenas; slots and
  skeleton tables are filled from the arena — the arena pointer is the only
  reference form (no native pointers are ever stored inside canonical text,
  which holds no pointers at all — verified: 0 interior pointers).
- `HOST_LOGICAL_ADDRESS_CAPACITY` (197 today) is **not** consumed by R13-C;
  R13-G will need ≈12,900 entries — noted here so R13-G pre-plans the
  capacity bump (host `.bss`, outside serialized slices).

## 8. State-v5

Serialized text-pointer surface, audited field by field:

| Holder | Serialized? | Interior pointers? | R13-C impact |
|---|---|---|---|
| `TextPrinter.printerTemplate.currentChar` (0–4 live) | yes — today **skipped and restored opaque** (`RuntimeLocationIsTextPrinterPadding`, native_state.c:1304) | **yes** — mid-string after any char/escape | **machinery edit**: route currentChar through the range walker when (and only when) the value lies inside a registered text range; opaque restore remains the fallback for compiled text. No format change. |
| Trainer speech buffers (9 × `u8*`, EWRAM) | yes (mid-battle saves) | no (string starts) | none in R13-C — point at script-side text (stays compiled) |
| `gApproachingTrainers[].trainerScriptPtr` | yes | no | none (script pointers, R13-G) |
| `sStringPointers[8]` / `gStringVar*` | yes | no — **copies**, not pointers into text | none (bytes copied into EWRAM) |
| Frontier facility speech (`gFacilityTrainers`) | yes (challenge lifetime) | no | none in R13-C (script-side text) |
| Battle-script registers | yes | n/a (script bytecode, not text) | R13-G |

The one real R13-C change: the currentChar routing above, plus range
registration for the cut-over family arenas. Interior offsets are already
expressible (v5 `rangeOffset` = byte offset within the range). The audit's
"zero interior pointers" holds for **source/static references**; the runtime
printer is the single interior-pointer class, and it is now designed for.

## 9. Per-resource ranges (capacity check)

**Per-label ranges are rejected** — they would bust the cap: 2,040 (existing
trainer/pokémon + audio) + 4,823 (C-side labels) + 362 (bundles) = 7,225,
leaving 967 slots for R13-D/E/F/G/H's projected 1,100–1,500 row ranges and
~600 script/map bundle ranges → **over 8,192**. Per the brief: redesign
granularity, don't raise caps.

**Chosen identity: one range per family arena.** The C-side families group
into **~15 arena ranges** (battle, move, ability, nature, item, pokedex,
system/shared, tv, matchcall, apprentice, ribbon, frontier, frontier-brain,
easy-chat, berry, misc ≈ 16); script-side bundles add up to 362 ranges at
R13-G (none in R13-C — their pointers never enter state while compiled).
The sidecar record is `(familyBundleKey, type=text, schema=1, role=2
compatibility-object, rangeOffset)`. Role 2 has precedent (compat objects
are not pack resources yet own ranges today); the range key is a
seam-registered bundle identity that round-trips through the range index
without a resolver lookup. Offset-in-range resolves the exact string via the
seam's deterministic sorted layout.

Projection: 2,040 + 16 (R13-C) = **2,056 ranges**; after R13-G (script
bundles live) ≈ 2,418; after R13-D/E row ranges ≈ 4,000. **Under cap with
~4,000 headroom.**

Sidecar impact: worst plausible save unchanged from the audit's 60-record
figure (currentChar adds 0–4 records, already counted in that estimate).
No State-v5 format change.

## 10. Runtime publication

New focused seam `src/emerald/resources/emerald_text_compat.{c,h}`
(`EmeraldTextCompat_`), sibling to the R13-B leaf seam and the R12 audio
seam, wired into `emerald_runtime_loader.c` after audio/leaf:

- **Transactional**: phase 1 validates every resource (resolve/ownership/
  type/schema/size/byte equality against the session's pack views, label
  count pins, escape-grammar revalidation, per-family composition pins)
  before any allocation; phase 2 allocates family arenas, copies payloads
  (sorted-name packing), fills slots + skeleton tables, then publishes
  atomically. Any failure keeps the prior state — a failed publication is a
  DEGRADE (compiled text serves exactly as pre-R13-C), never a refusal.
- **Deterministic arena layout** (sorted packing, 16-byte alignment —
  byte-identical across runs, runner-proven).
- **Exact ROM logical identity**: every record carries romOffset/size/
  provenance; the arena never stores native pointers; canonical payloads
  are immutable and permanent (session lifetime, no hot reload).
- **Named diagnostics** per entry (canonical name, stage, expected/actual
  size/schema — the R13-B diagnostics struct, with the 128-char buffer
  lesson applied).
- **Publication from the active resource session only** — no session, no
  seam (the R12-E refusal contract; additive families make this a
  fail-soft, cut-over families a fail-closed-documented degrade).
- Slot tables + skeleton tables are **generated** (`*_native.generated.c`,
  the R13-B `leaf_native_table` pattern); the seam fills, never hand-edits.

## 11. Live cutover strategy — D (hybrid by family)

Chosen: **publish everything; cut over the C-side in R13-C; defer the
script-side to R13-G.**

| Tranche | Families | Labels | Bytes | Mechanism |
|---|---|---|---|---|
| R13-C live (skeleton tables) | battle strings, move/ability/nature descriptions, contest effect text | ~1,000 | ~32 KB | generated skeleton tables, zero consumer edits |
| R13-C live (slots) | shared/system/tv/match-call/apprentice/ribbon/frontier/frontier-brain/mauville-man/news/rating/misc | ~3,700 | ~150 KB | generated pointer slots + skeleton tables, zero call-site edits |
| R13-C additive | script-side bundles (maps/trainer/misc-scripts/…) | 7,933 | 747 KB | pack + arena + provenance; compiled fallback live |
| R13-C additive (table-blocked) | item/pokedex/berry/easy-chat/cable-club(partial) | ~1,800 | 81 KB | published; repointed when gItems/gPokedexEntries/gBerries/easy-chat blobs migrate (R13-D/E/leaf) |
| Exempt (11 symbols) | double-referenced | 11 | ~0.5 KB | stay compiled arrays through R13-G, documented |

This minimizes the long tail without touching scripts: the COMPILED tail is
bounded to exactly the families whose cutover is architecturally owned by
R13-D/E/G, with per-symbol documentation — the R13-B precedent.

## 12. Isolation

Per family, after cutover (both flavors, fresh `-B`):

1. **Symbol absence** for skeleton-table families (`s*Description`,
   `sText_*`, `gMoveDescriptionPointers` rows): `nm` class sweep, zero hits
   required.
2. **Symbol-shape audit** for slot families: the legacy string symbols are
   absent as `.rodata`/`.data` **objects**; the same names exist only as
   `HOST_DATA` pointer slots (section-scoped `nm` proof), and the generated
   ownership table lists each with `SLOT` disposition.
3. **Payload byte-scan** against both binaries for every resource with a
   payload ≥ 16 B (measured median 49 B — the scan has real power);
   payloads 3–15 B are scanned with a documented high-false-positive NOTE
   class; ≤ 2 B payloads (19+50 labels) are exempt-by-length — a
   per-length **class** rule, never a blanket "text exempt" (each label is
   still symbol-checked).
4. **Additive families** keep `COMPILED_PENDING_MIGRATION` ownership with
   presence proofs (symbols must be present — the R13-B movement pattern,
   full `nm --defined-only` for local labels).
5. **Alias counting**: the 476 duplicate groups are counted once per label
   (no content-level collapsing); the 11 exempt symbols are individually
   documented exceptions with reasons.
6. Short-string collision NOTE reporting follows the R12-G runner grammar
   (reported, not failed — only sha-keyed classes).

## 13. ROM-hack import implications

| Hack change | Maps to | Works alone? |
|---|---|---|
| One NPC line (`msgbox` in one map's scripts.inc) | override `emerald:text/map/<Map>` (rebuilt bundle) — or, once scripts migrate, the per-map script bundle carries its text | yes — one bundle replace |
| One item description | `emerald:text/item/<id>` | yes — single-entry MOD override (exact-size constraint enforced) |
| One move/ability/nature description | `emerald:text/move/<id>` etc. | yes |
| One Pokédex entry | `emerald:text/pokedex/<species>` | no — until R13-E repoints `gPokedexEntries`; the hack's change is meaningful only with the R13-E table |
| One battle/system string | `emerald:text/battle/<symbol>` / `emerald:text/system/<symbol>` | yes (slots + tables rebuilt at publish from the resolved view) |
| Easy-chat word | `emerald:text/easychat/<symbol>` | after the word-blob leaf migration (blobs are rebuilt from words) |
| Trainer dialogue (trainerbattle intro/lose) | `emerald:text/data/trainers` bundle (per-trainer labels inside) | with the script stage (R13-G) — text and script travel together |
| Dialogue that changes script flow | text bundle + script bundle (R13-G) | no — script-dependent text changes are only meaningful with script import |

The resolver precedence is untouched: MOD > ROM_BASE, invalid override
falls back with trace evidence. Per-label C-side identity is exactly the
granularity the mod contract needs; per-map bundles match how ROM hacks
actually edit maps (rebuilt scripts.inc through the same generator).

## 14. Failure matrix (fail-closed)

All injection tests on the seam (the R13-B harness shape), plus text-specific
cases:

| Case | Injection | Expected |
|---|---|---|
| E1 | family missing from pack (count pin) | UNEXPECTED_COUNT |
| E2 | one label dropped | UNEXPECTED_COUNT |
| E3 | re-schema'd entry | UNEXPECTED_COUNT (or SCHEMA_MISMATCH) |
| E4 | wrong type | TYPE_MISMATCH |
| E5 | payload truncated vs session | PAYLOAD_SIZE_MISMATCH |
| E6 | missing `0xFF` terminator (payload cut/patched) | PAYLOAD_INVALID (escape-grammar revalidation) |
| E7 | out-of-ROM slice (bad romOffset) | ROM_SLICE_MISMATCH |
| E8 | two entries claim one ROM slice | OVERLAPPING_SLICE |
| E9 | alias mismatch (legacySymbol ≠ ELF) | TABLE_MISMATCH |
| E10 | malformed control code (dangling F8/F9/FC/FD lead) | PAYLOAD_INVALID |
| E11 | bad logical mapping (arena offset beyond blob) | range-index registration refusal / capture error |
| E12 | State-v5 missing key/offset (currentChar routing) | capture fails with diagnostic, never silently omits |
| E13 | slot publish with cleared session | UNAVAILABLE refusal, no stale pointers |
| E14 | duplicate canonical name | writer GEN3_PACK_ERR_DUPLICATE_NAME |

No silent compiled fallback after a family is declared live ROM-backed
(the R12-E contract; additive families degrade to their documented compiled
state, which is the declared pre-cutover behavior).

## 15. Implementation order

1. Text-family generator (enum-position bindings, bundles + labels,
   legacySymbols, dup-group metadata) with `--check` determinism.
2. Catalog + manifest + pack extension (5,185 entries); `gen3-pack-build
   --check` byte-for-byte proof (the R13-B E1 pattern).
3. Publication seam + generated slot/skeleton tables; failure matrix (E1–E14).
4. Family-arena ranges + currentChar walker routing (State-v5, §8–9).
5. Cutover tranche 1 (skeleton families), then tranche 2 (slot families),
   each with full-battery + build-parity gates.
6. Isolation runner extension (text ownership classes, §12).
7. Full 35-suite regression + R13-C runner; fresh `-B` release/DINFO builds
   with size deltas.
8. Report (`docs/R13C_TEXT_MIGRATION_REPORT.md`).

**Split recommendation: implement as one stage (R13-C) with two cutover
tranches.** The seam/state work is shared; the tranches differ only in the
generated-table shape. No R13-D work may start before tranche 2 lands
(item/pokedex strings are additive and R13-D/E depend on the seam).

## 16. Projected totals

| Quantity | Value |
|---|---|
| Text labels (source-defined) | 12,756 (12,755 after the 1 host-hydrated exclusion — `gSpeciesNames`) |
| Canonical bytes into the pack | 901,781 B |
| New resources | **5,185** (362 bundles + 4,823 labels) |
| Pack entries | 6,876 → **12,061** |
| Pack size | 10,076,400 → ≈ **12.0–12.3 MB** (payload + TOC + names; exact after E1) |
| Cut over live in R13-C | ≈ 4,700 labels / ≈ 180 KB (skeleton + slot families) |
| Additive in R13-C (deferred) | ≈ 8,050 labels / ≈ 722 KB (script-side + table-blocked + 11 exempt) |
| Release binary delta | ≈ **−70 to −90 KB** net (removes ~150 KB C-side `.rodata`; adds ~4,700 × 8 B slots ≈ 38 KB + skeleton tables + seam/generated code) |
| Range index | 2,040 → **2,056** in R13-C (16 family-arena ranges); no cap concern |
| Sidecar | +0–4 records/save (currentChar); format unchanged |
| Complexity/time | Medium-high (generator + seam + slot machinery + one walker edit). ≈ **2–3 weeks** including both tranches and the full battery. |

**R13-C is safe to start on this plan. STOP here — no implementation, no
commit, no R13-D.**
