# R13 — Content Ownership Architecture

Design for the R13 migration (text, scripts, map metadata, gameplay data,
and remaining structured tables → ROM_BASE resources). Grounded in the R13-A
audit (`docs/R13_REMAINING_CONTENT_AUDIT.md`); its decisive numbers: ≈**3.6 MB
of compiled original-game payload remains** (3,226,420 B `.rodata`/`.data`
payload + 176,352 B multiboot + 1,065,814 B `script_data`), plus a
**second-identity content package** (species/move/name/exp tables + fonts)
that must be absorbed into the Gen3 registry. This document defines the
classifications, vocabulary, dependency graph, State-v5 model, cutover
seams, stage subdivision, isolation approach, and ROM-hack import
implications. No production code was modified during this design.

**Status (2026-08-18):** R13-B (movement scripts 7,428 B + multiboot
programs 176,352 B) complete — those payloads now live in the ROM_BASE
pack behind the additive leaf seam, and the design-time "remains" figures
above are reduced accordingly (see
`docs/R13B_LEAF_PAYLOAD_MIGRATION_REPORT.md`).

**Status (2026-08-25):** R13-B through **R13-J COMPLETE** — every R13
content family is migrated and isolated, and the final closure (R13-J)
is green: global ownership census unexplained 0, COMPILED_PENDING
sweep 0 unexplained (1,388 explained: movement STAY bridge 15,
multiboot 2, easy-chat engine carve-out 1,371), §3A FLIP removed the
last uncompensated compiled payloads (1,040 movement tables, 11 text
blocks, 2 labels — all pack-served, arbiter-proven), State-v5
unchanged (5u), ranges 6,390/8,192, pack 23,069
(`b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb`),
release 23,464,296 B / DINFO 36,105,696 B both ROM-verify
(f3ae0881…d07b7), isolation 7,908/7,908, full battery green including
the state-master harness three-mode flake closure (mapScripts window
planted NULL). See `docs/R13I_STATE_V5_CLOSURE_REPORT.md`,
`docs/R13I_STATE_V5_POINTER_CENSUS.md` and
`docs/R13J_FINAL_OWNERSHIP_ISOLATION_REPORT.md`. R14 not begun (handoff
list in §11).

**Status (2026-08-25): R14 AUDIT COMPLETE — STOP with finding.** The
R14 whole-project audit (`docs/R14_FINAL_EMERALD_OWNERSHIP_AUDIT.md`)
proved every *migrated* family cleanly isolated on the whole binary
(37,681/0 isolation, canonical-byte hunter A=0, 0 stale/0 unexplained
among declared families, clean rebuild byte-identical, State-v5/6,390
ranges green, zero build deltas) and discovered the remaining
un-owned canonical surface: **~1.36 MB of gfx/text compiled with no
pack ownership** (mon icons 430,080 B, still-fronts 363,816 B,
battle-anim gfx 118,988 B, item icons 57,896 B, wallpapers 35,700 B,
UI/misc 322,000 B, footprints 12,384 B, credits/union-room/decoration
text 20,863 B) plus easy-chat (1,008 words, engine carve-out) and the
2 multiboot programs (incl. one full Nintendo-published GBA ROM
image). Per brief §34 (substantial new content families) R14 STOPs
with the exact inventory as the **R15 "gfx-leaf + UI/text closure +
easy-chat/multiboot cutover"** scope. R14's one narrow fix: the 27
C-owned text labels (~857 B) now carry a `stay-compiled` consumer
declaration. No commit; FireRed/Tallgrass not begun.

## 1. Scope decision: what belongs in R13

R13 takes **all remaining original-game payload** — text, script bytecode,
map metadata, gameplay tables, and the gfx-leaf blobs that the earlier waves
left behind. The gfx leaves are included even though they are "graphics":
the R9 machinery (tile-graphics/palette types, compat tables, isolation
runner) already exists for them, they are the single largest byte volume,
and deferring them to R14 would violate the R14 boundary rule ("R14 = final
global isolation + tiny edge cases").

**Out of R13 (R14 only):** engine residue — interpreter command tables
(`gScriptCmdTable`, `gBattleScriptingCommandsTable`, …), `gSpecials`/
`gStdScripts`/`gSpecialVars` (function pointers), compat-seam descriptors
(`kPokemonBattleCompatResources` 38,592 B, `sPaletteRows`, `sLayoutRows`),
`host_data` renderer scratch (7.96 MB), serialized-slice state, and
sub-200-byte routing tables where identity cost exceeds benefit (see §5-D).

## 2. Pointer-shape classification (drives implementation order)

| Family | Shape | Rationale |
|---|---|---|
| Movement scripts | **A LEAF_BYTES** | 7,428 B of pure 1-byte opcodes; **zero interior pointers**; only inbound `applymovement` refs — **migrated in R13-B** |
| Multiboot programs | **A LEAF_BYTES** | two standalone blobs (176,352 B), no pointer graph — **migrated in R13-B** |
| Braille font, weather palette table, bard templates, easy-chat word tables, type chart, heal locations, learnset leaves, TMHM bitfields, egg moves, encounter mon rows | **A LEAF_BYTES** | numeric rows only |
| Gfx leaves (icons, still pics, item icons, berry pics, battle anim sprite gfx, door/weather/transition blobs, wallpapers) | **A LEAF_BYTES + compiled pointer tables** | payload leaves; `gMonIconTable`/`gItemIconTable`/… are the R9-pattern tables to repoint |
| Text | **A payload + logical-address registry** | leaf charmap bytes; every reference is label-addressed via `HostResolveGbaAddr` — the R12 audio cutover pattern, zero consumer edits |
| Species/move/item/trainer/frontier/encounter rows | **B (numeric) / C TRANSFORMED_STRUCTURAL (mixed)** | numeric rows migrate as canonical wire; `gItems` mixes text ptrs + **engine fn pointers** (use functions stay compiled — the R12-C voicegroup pattern: split data fields from engine fields) |
| Map headers/events/connections | **B GBA_POINTER_GRAPH** | canonical GBA form is pointer-bearing; native form is already host-width — migrate as per-map records + republish |
| Event/battle/anims/AI scripts | **B GBA_POINTER_GRAPH** | canonical bytecode embeds 4-byte logical addresses; native today embeds link-time native pointers (16,651 + 4,231 + … operands) — needs relocation metadata at hydration |
| `gMapLayouts`/`gMapGroups`/`gMapGroup_*`, cry-ID, species→dex routing | **D INDEX_ROUTING** | 4-byte logical IDs only; may remain compiled (2,208 + 1,764 + 1,092 B total) |
| Command tables, `gSpecials`, compat descriptors, use-fn pointers, `host_data` scratch | **E ENGINE_CONSTANT** | not original payload; remain compiled |

## 3. Resource vocabulary and identity model

Types: reuse `TEXT` (currently unused) for text; add `SCRIPT` (schemas:
`field-event`, `battle`, `battle-anim`, `battle-ai`, `contest-ai`,
`movement`, `mystery-gift`, `field-effect`) and `STRUCTURED_DATA` (schemas
per family: `species-base`, `species-learnset`, `species-evolution`,
`move-battle`, `move-contest`, `item`, `trainer`, `trainer-party`,
`encounter-wild`, `frontier-facility`, `pokedex-entry`, …) — or, where a
family is pure bytes, `BINARY` with family schemas. Type/schema codes are an
implementation detail of the cutover stages; the identity rules below are
the contract.

**Identity granularity (per §8 mod layering, grounded in the audit):**

| Family | Identity | Why |
|---|---|---|
| Text | **per-map / per-file blob** (`emerald:text/map/<map>/…`, `emerald:text/data/<file>/<label>`), individual stable labels for shared strings | every reference is a label → a whole-blob identity preserves all label offsets; per-string identity for the 632 shared `gText_` pool gives mods stable string overrides |
| Scripts | **per-map bundle** for field scripts, **per-root** for battle/anim/AI families | `call`/`goto` targets stay label-relative inside a bundle (relocation records); mods replace a map's script graph atomically |
| Map metadata | **per-map record bundle** (header + events + connections) | map-scoped mods; keeps the 4-pointer header coherent |
| Species | **per-species** rows (base, learnset, TMHM, evolution, egg moves) | species-scoped mods (stats/learnset edits are the #1 ROM-hack use) |
| Moves | **per-move** rows (battle + contest) | move-scoped mods |
| Items | **per-item** row | item-scoped mods (price/desc/held-effect) |
| Trainers | **per-trainer** (metadata + party) | trainer-scoped mods |
| Encounters | **per-map encounter set** | map-scoped wild tables |
| Frontier | **per-facility bundle** | facility-scoped mods |
| Gfx leaves | **per-asset** (the existing R9 naming style) | asset-scoped mods |
| Multiboot | **per-program** | leaf blobs |

Keys remain derived by the M0/M1 rule
`SHA-256("gen3-resource-id-v1\0" + exact canonical-name bytes)` — no opaque
numeric keys. The stable names double as the ROM-hack import surface.

**ROM-hack import consequence (§15):** an existing ROM hack maps onto
overrides naturally: species/move/item/trainer row edits → per-row MOD
entries; map/script edits → per-map script/text bundles (the hack's
rebuilt `scripts.inc` feeds the same generator); gfx edits → per-asset
entries. Items' `fieldUseFunc`/`battleUseFunc` are engine mechanics — a hack
changing them needs an engine adapter, not data (classified
"requires known engine mechanic adapter"; everything else is
"straightforward data override" or "script graph import").

## 4. Dependency graph (drives stage order)

```text
        gfx leaves ────────────────────────────┐ (R9 machinery, no deps)
        movement / multiboot / leaf tables ────┤
                                               ▼
   text blobs ──────┐                 leaf payloads out first
                    │
   species/move/item tables ──► content.pak absorption ──► fonts
                    │
   encounter/trainer/frontier tables ──┐
                                        │
   map metadata (headers/events/connections/region) ──► needs text+scripts identity
                                        │
   scripts (field → battle/anims/AI) ──► needs text + map metadata + movement identity
```

Migration-order rules:

1. **Leaf families first** (no consumers to redirect — the R12-B pattern:
   pack → arena → provenance → ownership before any consumer edit).
2. **Text before scripts** (scripts' largest operand class is text: 9,485
   references; text has zero interior pointers and its reads already funnel
   through `HostResolveGbaAddr`).
3. **Tables before map metadata** (map headers point at encounter headers;
   `gWildMonHeaders` points at encounter tables).
4. **Map metadata before field scripts** (headers/events point into script
   bundles; coord/BG events carry script pointers).
5. **Field scripts last among data families** (highest operand count, the
   `sAddressOffset` v-address coupling, interpreter state split across host
   .bss + serialized EWRAM).

## 5. Live cutover seams (minimum per family — no broad engine rewrites)

| Family | Seam | Consumer edits |
|---|---|---|
| Text | register text labels in the `HostResolveGbaAddr` exact-start logical registry (`HostMemoryRegisterLogicalAddress` — today used only by the R12 audio seam); republish the pointer tables (`gStdStrings`, `gPokedexEntries`, description tables, `gAbilityDescriptionPointers`) | **zero** — `ScriptReadPointer`/`T1_READ_PTR`/`T2_READ_PTR` already resolve |
| Scripts | same logical registry for script-table labels; hydrator patches native-width operand tables from canonical 4-byte logical form + relocation records | **zero** — operand reads already go through `HostResolveGbaAddr` |
| Species/moves | extend the existing content-package hydration (`desktop_game_content.c`) with Republish-style repointing (the R9 pattern); absorb `content.pak` into ROM_BASE so species/move/name/exp tables + fonts become ordinary resources | table-array repoint only |
| Items | Republish/alias `gItems` (thin accessors already exist: `GetItemName`/`GetItemPrice`/`GetItemHoldEffect`/…) | none for field access; use-fn pointers stay compiled |
| Trainers/encounters/frontier | Republish the arrays; consumers are transient (no cached row pointers anywhere) | none |
| Map metadata | existing PORTABLE seam (`Overworld_GetMapHeaderByGroupAndId` + `GetMapLayout`); republish the 518 headers / 441 layouts / events / connections; `gMapGroups`/`gMapLayouts` stay compiled 4-byte index tables (shape D) | fieldmap.c's direct `gMapHeader.` consumers keep working against the EWRAM copy |

## 6. State-v5 model

No format or machinery changes. The v5 walker already captures every
serialized pointer by range lookup; migrating families only switches the
record kind from image-relative to sidecar. New sidecar contributors:

- `gMapHeader` EWRAM copy: 4 pointer fields (mapLayout/events/mapScripts/
  connections) — always live in the field.
- Script contexts: `sMysteryEventScriptContext` (full ScriptContext),
  `gBattlescriptCurrInstr`, `gAIScriptPtr`, `sBattleAnimScriptPtr`/`RetAddr`,
  `gRamScriptRetAddr`, battle script stacks (heap→EWRAM slice).
- Trainer speech buffers (9 × `u8*`, cleared at battle end),
  `gApproachingTrainers[2].trainerScriptPtr`.
- Active `TextPrinter.currentChar` (0–4; already specially modeled),
  `gFonts`, `sStringPointers[8]`.
- Frontier: `gFacilityTrainers`/`gFacilityTrainerMons` (2 records for the
  challenge lifetime).

Projection: typical field save ≈ 6–14 records; mid-battle ≈ 10–35; worst
plausible ≈ 60 — versus the 4,096 cap (≈1,005 used by audio today). Species/
move/item/trainer/encounter tables have **zero serialized pointer surface**
(transient consumers; daycare copies u16 move IDs, not pointers). **No cap
changes required.**

Range-index arithmetic (cap 8,192; ≈3,000 registered today): R13 publishes
at bundle granularity (per-map text/script bundles, per-family table arrays,
per-asset gfx) ≈ 3,000–3,500 new ranges → ≈6,000–6,500 total. Per-row ranges
are reserved for families whose pointers enter state or that need per-row
mods (≈1,100–1,500 rows across species/moves/items/trainers at most). The
index stays within cap without changes; if a future stage needs more,
extend the cap then — arithmetic must show need first.

## 7. Proposed R13 subdivision

| Stage | Families | Resources (approx) | Canonical bytes | Risk | Prereqs | Cutover |
|---|---|---|---|---|---|---|
| **R13-A** (done) | audit + vocabulary | — | — | — | — | — |
| **R13-B** leaf payloads | movement scripts, multiboot programs, braille font, weather palettes, bard templates, easy-chat words, learnset/egg/TMHM/tutor leaves, gfx leaves (icons, still pics, item/berry icons, battle-anim sprite gfx, door/weather/transition/wallpaper blobs) | ≈ 2,800 | ≈ 2.0 MB | low — R9/R12 machinery exists; no consumers to redirect | none | in-stage (pack→arena→provenance→ownership, then table repoints) — **movement + multiboot DONE (2026-08-18); the remaining leaf families follow the same seam** |
| **R13-C** text | all text families (874 KB) + pointer-table republish | ≈ 700–1,200 blobs (per-map/file) | 874,022 | medium — the highest-value mod target; zero interior pointers make it safe | R13-B vocabulary | in-stage (logical-address registry; live immediately) |
| **R13-D** gameplay data | species (learnsets/evolution/TMHM/egg/tutor/names), moves (battle/contest/desc), items (data/desc/effects), fonts; **content.pak absorption** | ≈ 2,300 rows + 15 content entries | ≈ 120 KB + 26 KB absorbed | medium — consumers direct-index; Republish seam; content.pak retirement is a loader lifecycle change | R13-C text (desc pointers) | in-stage (Republish) |
| **R13-E** trainers/encounters/frontier | `gTrainers`+parties, wild encounter tables + headers, frontier facilities, pokedex entries, contests, match call, decorations, berries, landmarks, TV, secret base, small tables | ≈ 3,500 rows/tables | ≈ 175 KB | low-medium | R13-D schema patterns | in-stage (Republish) |
| **R13-F** map metadata | 518 headers, 441 layouts, events (2,776 object-event templates, warps, coord/BG events), connections, region map, map names | ≈ 700 per-map bundles | ≈ 181 KB | medium — pointer-bearing; needs text+script identity first | R13-C, R13-E (encounter headers) | in-stage (Overworld seam + Republish) |
| **R13-G** field/event scripts | 7,272 entry scripts + 470 map script tables (975 KB object) | ≈ 468 per-map bundles | ≈ 340 KB bytecode | **highest** — 16,651 operands, `sAddressOffset`, split interpreter state | R13-B movement, R13-C text, R13-F map metadata | in-stage (logical registry + sidecar suite) |
| **R13-H** battle/anim/AI scripts | battle scripts, anim scripts, AI scripts, contest AI, field-effect scripts, mystery-gift scripts | ≈ 1,800 roots | ≈ 91 KB | medium-high (mid-battle saves add sidecar records) | R13-G VM patterns | in-stage |
| **R13-I** State-v5 closure | cross-restart suites for script/text/map pointers; load hooks; sidecar matrix | — | — | medium | R13-G/H | in-stage |
| **R13-J** binary isolation | ownership records for all new families; symbol/byte-scan proofs on fresh -B release + DINFO; GBA preservation proofs; size report | — | — | low-medium | all above | final |

First implementation target was **R13-B** (leaf payloads) — the
movement-script family is the ideal opening move (1,055 roots, zero
interior pointers, 7,428 B) alongside the multiboot blobs (single-range
leafs), proving the new `SCRIPT`/`BINARY` schema plumbing on
trivially-safe families before text. **COMPLETE 2026-08-18** (additive
seam, failure matrix, isolation battery, runner — see
`docs/R13B_LEAF_PAYLOAD_MIGRATION_REPORT.md`); the remaining R13-B leaf
families (braille font, weather palettes, bard templates, easy-chat
words, learnset/egg/TMHM/tutor leaves, gfx leaves) reuse the same seam
and pack pipeline.

## 8. Isolation approach

Per family, after cutover, prove: qualified GBA ELF bytes == retail ROM slice
== pack payload == arena bytes, then **payload absent from fresh -B release
and DINFO binaries** (symbol absence, per-object scan, byte scan, dry-run
dep-graph, GBA-branch source inspection — the existing runner grammar).

Byte-scan difficulty by family:

- **Text:** individual strings are tiny and heavily repeated (1.79% exact
  duplicates today, plus many short phrases) — byte-scan at string
  granularity is weak; the proof is **symbol absence** (all 12,600+ labels
  absent) + blob-level sha scans at bundle granularity + the logical
  registry holding no compiled addresses. No blanket exemptions; bundle-sha
  exemptions only for provable coincidences.
- **Scripts:** blob-sha scans work (blobs are unique); short 1-byte-opcode
  runs (movement) will collide — symbol absence + blob sha are the proof;
  sub-1 KB rows fall under the existing runner NOTE rule (not FAIL).
- **Numeric tables:** rows of 6–72 B are below meaningful scan length —
  symbol absence + object-level nm scans; whole-table sha scans.
- **Zero-heavy tables:** sha over the exact table with length and base
  address provenance (sha-keyed records, the R12 pattern) — the
  "coincidence classes" documented per family, never a section-wide or
  size-only exemption.

## 9. Encoding policy for R13 families

- **Text:** keep **exact GBA charmap bytes** (0xFF-terminated, control codes
  intact) as the canonical payload — the architecture doc's standing rule
  ("original Emerald charmap bytes, direct permanent pointer"). **No UTF-8
  conversion** in R13: it would break byte-parity, the script operand
  pipeline, and the ROM-hack import surface. (A converter can be a mod-tool
  concern later; the engine must not be rewritten.)
- **Scripts:** canonical = original bytecode with 4-byte logical address
  operands + relocation records (the R12-D/E pattern); hydration patches
  native-width operand tables.
- **Structured tables:** versioned canonical little-endian wire per family
  schema (never host structs); republished to host-width rows.
- **Gfx leaves:** existing R9 encodings (decoded tiles/palettes, original
  wire for raw data).

## 10. Whole-R13 estimated totals

| Quantity | Value |
|---|---|
| Remaining migratable payload (compiled) | ≈ 4,468,586 B (3,226,420 `.rodata`/`.data` + 176,352 multiboot + 1,065,814 `script_data`) |
| Canonical byte volume to enter the pack | ≈ 4.2 MB (payload minus engine-side residue + per-family wire) |
| Resource count at recommended granularity | ≈ 8,000–10,000 entries (2,800 gfx + 700–1,200 text blobs + 2,300 gameplay rows + 3,500 trainer/encounter/misc + 700 map bundles + 2,200 script bundles/roots) |
| Pack growth | 9,644,320 B → ≈ 13.8 MB (5,819 → ≈ 15,000 entries) |
| Binary reduction | release 20,952,576 → ≈ 16.5 MB (−21%); DINFO similarly |
| Range index after R13 | ≈ 6,000–6,500 of 8,192 (no cap change) |
| Sidecar after R13 | ≤ 60 records/save added (≤ 4,096 cap, no change) |
| Serialized pointer surface added | `gMapHeader` 4 + script/text registers (per §6) |

## 11. R14 boundary (explicit)

R14 contains **only**: (1) final global isolation proof and link-ownership
report across both binaries; (2) the remaining engine residue classified E
(command tables, `gSpecials`/`gStdScripts`/`gSpecialVars`, compat seam
descriptors, `host_data` scratch) — these are **not** migrated, only
re-classified; (3) sub-200-B shape-D routing tables explicitly allowed to
remain compiled; (4) packaging/legal-content audit; (5) cleanup
(packaging, docs, source-tree overlay strategy per the migration
architecture §15). **No major content family is deferred to R14** — gfx
leaves, text, scripts, map metadata, and all gameplay tables are R13.

## 12. Highest-risk items (ranked)

1. **Field-script cutover (R13-G)** — 16,651 native pointer operands in one
   975 KB object, the `sAddressOffset` self-relative v-address scheme, and
   interpreter state split across host .bss + serialized EWRAM. Mitigation:
   exact R12-D/E relocation-metadata machinery; the cutover is per-map
   bundle with a side-by-side LEGACY_COMPILED oracle until the family sweep
   is clean.
2. **content.pak absorption (R13-D)** — retiring the second identity
   package while species/moves remain the most-indexed tables in the game;
   mitigation: republish-over-hydration (the tables keep their exact host
   layout; only the source of bytes changes).
3. **Mid-battle save/load with arena scripts (R13-H/I)** — battle script
   registers enter the sidecar; mitigation: the R12-F save-load suite
   pattern (control-vs-test PCM/state equality per scenario).
4. **Per-map identity stability** — map names/labels must not drift across
   stages; mitigation: generator emits both GBA tables and extraction
   bindings from one family descriptor (the existing generated-ownership
   pipeline).

## 13. Stop conditions

STOP a stage if: a migrated payload remains compiled in release or DINFO
without a documented structural exception; a live pointer falls back to
removed compiled data; any PCM/state delta appears in the release flavor;
State-v5 ranges/hulls/counts change unexpectedly; startup with a valid pack
fails; invalid/missing pack crashes instead of refusing; the GBA data path
changes in a new way; or the stage requires an engine rewrite (script VM
opcode changes, text re-encoding, item/move struct changes) instead of
relocation/republication. R13 must not change any resource identity,
State-v5 format, or provider precedence.

## 14. Open items to resolve before the first cutover stage

- The five revision-delta flags in the audit §5.5 (wild maps 125 vs 124,
  WildPokemonInfo 209 vs 220, wild rows 2,070 vs 2,107, PikeWildMon layout,
  easy-chat row layout) — resolve which side is authoritative before any
  extraction manifest is cut.
- The 68,576 B `event_object_movement.o` `.rodata` residual beyond the
  7,428 B movement-script family (NPC sight/behavior tables) — classify
  before R13-E (the movement family itself is R13-B-complete).
- Exact `TEXT`/`SCRIPT`/`STRUCTURED_DATA` type-code and schema assignments
  (an R13-B deliverable, not decided here).
