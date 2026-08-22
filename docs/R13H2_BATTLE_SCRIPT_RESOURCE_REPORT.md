# R13-H2 — Battle Script Module Resource Report

Status: **complete**. This was an additive resource-generation stage only.
No live battle/animation/AI execution was modified, no H arenas were
created, no H resources were published, no State-v5 ranges were registered
(live range count stays 6,377), no runtime behavior was changed, nothing
was committed.

All bytes below are the qualified BPEE01 ROM
(`f3ae088181bf583e55daf962a92bb46f4f1d07b7`) — the compiled payloads stay
the live runtime source through H2 (§28). Where the plan (§18) estimated
pack arithmetic, the qualified truth is documented and the old number
reconciled — it was not forced.

Generated artifacts live under
`resources/extraction/emerald/bpee01/battle/modules/` (manifest + 10
sidecar TOML files + `meta/` ×2,089 + `data/` ×2,081); all are `--check`
byte-identical on regeneration. Generator: `tools/gen3_resources/battle_family/`
(h2_generate.py + h1_* analyzers reused verbatim); staging harness:
`tools/gen3_resources/battle_family/h2_stage.py`.

## 1. Deliverables (brief §1, §2)

Exactly **2,089 canonical module resources** across five families, keys
`emerald:<family>/<module>` with the schema pins 47–51:

| Family | Key prefix | Modules | Payload records | Canonical bytes | Schema |
|---|---:|---:|---:|---:|---:|
| Battle scripts | `emerald:battle-script/` | 645 | 641 | 13,592 | 47 |
| Battle anim scripts | `emerald:battle-anim-script/` | 658 | 654 | 63,811 | 48 |
| Battle AI scripts | `emerald:battle-ai/` | 553 | 553 | 9,303 | 49 |
| Contest AI scripts | `emerald:contest-ai/` | 165 | 165 | 2,524 | 50 |
| Field-effect scripts | `emerald:field-effect-script/` | 68 | 68 | 817 | 51 |
| **Total** | | **2,089** | **2,081** | **90,047** | |

The 8 catalog-only identities are the **zero-width alias modules** — four
battle-script effects (`effect-morning-sun`, `effect-mud-sport`,
`effect-protect`, `effect-return` — aliases of their synthesis/mud-sport
base tables) and four battle-anim moves (`move_mirror_move`,
`move_nature_power`, `move_none` — alias bases of move_pound/mirror-move
animations) that share a base module's GBA address and therefore carry no
canonical bytes. They exist as full catalog/ownership identities so the
alias name→address mapping is first-class, but cannot be pack records.

## 2. Canonical bytes (brief §4, §24)

Every payload module is byte-for-byte the qualified ROM slice, proven by a
**four-way oracle**: ROM slice == ELF relocation result == H1 graph record
== H2 generated resource (7,549/7,549 checked, 0 bad; resource bytes ==
ROM slices). `sha256(payload) == source_encoded_sha256 ==
canonical_decoded_sha256` for all 2,081 records (source encoding `raw`, so
encoded == decoded by construction). No byte is rewritten, re-encoded, or
patched.

## 3. Relocations (brief §5, §21, §22)

All **7,549 relocation sidecar rows** are present (one per ELF relocation
operand, all width 4):

- `raw_encoded_value == addend` (the H1 `stored` REL addend) — 7,549/7,549;
- the **canonical word at every reloc site == the ROM word == `final_gba`**
  (the staged host word matches at both shadow bases) — 7,549/7,549;
- the complete partition by target class: **3,798 SCRIPT_TARGET + 3,751
  ENGINE_***, all resolved through the semantic binding table — no
  anonymous address survives. Of the ENGINE rows, 488 are EWRAM
  struct-field rows (23 unique structs); 2,778 rows are section-relative
  (object base = family arena gba_start + addend == final_gba);
- the source relocation index (§21) maps every host word back to its
  (module, offset, width) source record; the target binding index (§22)
  resolves SCRIPT_TARGET/ENGINE ids into canonical addresses with **no
  id-routing into pointers** — ids never ride inside canonical bytes.

## 4. Targets (brief §13, §15)

- **SCRIPT_TARGET: 3,798 targets, all on root spans, interior 0** — every
  script target is a module export; `target_offset ∈ export payload
  offsets`, export-name membership per (module, offset) (aliases share
  offsets legitimately: `Move_NONE`/`Move_POUND` both export at
  `move_pound` offset 0).
- **ENGINE: 3,751 targets** all bound by the semantic table; binding row
  identity = (gba_base_symbol, encoded_gba_value, addend) — EWRAM struct
  fields share their struct base with distinct field-offset addends
  (gBattleCommunication 5, gBattleMoveDamage 4, gBattleScripting 21).
- Semantic binding tables A–F: **50 / 50 / 327 / 213 / 11 / 67 = 718 rows**
  (EWRAM=A, TABLE=B, SPRITE_TEMPLATE=C, GFX=E), 488 EWRAM field rows over
  23 unique structs. No host pointer appears in canonical bytes.

## 5. Boundary maps (brief §14)

Deterministic instruction-boundary maps on 2,081 payload modules, tiles
covering `[0, byte_count)` exactly with 0 bad tiles: **bytecode 2,043 /
data 25 / routing 13 / empty 8**. The 25 data modules carry no relocs;
boundaries never straddle a reloc operand (checked per row).

## 6. NULL sentinels and addends (brief §16, §17)

- **65 NULL sentinels preserved unresolved** — byte-identical NULL words
  in the canonical bytes, never rewritten to host addresses, disjoint
  from all 7,549 reloc sites;
- **3,058 nonzero addends preserved verbatim** (2,778 section-relative +
  280 EWRAM fields), never folded into a base.

## 7. Ownership (brief §3, §27)

Ownership manifest: **2,089 × COMPILED_PENDING_MIGRATION**, per-target
`{native: COMPILED_PENDING_MIGRATION, gba: COMPILED}` — the compiled ROM
payloads remain the live runtime source; nothing is staged live. Enforced
by the loader test's ownership gate (2,089/2,089) and the staging harness.

## 8. Pack import (brief §25, §26)

The battle family is the 12th manifest+catalog pair in the production pack
build (`tools/gen3_resources/pack_build/build_d1_pack.sh`).

| Quantity | Value |
|---|---:|
| Pack entries (header offset 28) | **23,069** |
| Pack size | 15,278,272 B |
| Pack SHA-256 | `b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb` |
| Determinism | two independent builds byte-identical; `--check` green |

**Reconciliation (plan §18 → qualified truth).** The plan estimated
20,988 + 2,089 = 23,077 entries ("If the entry count differs: STOP and
reconcile"). Qualified truth: the 8 zero-width alias modules carry no
payload and cannot be pack records, so the pack entry count is
**23,069 = 20,988 + 2,081** (the catalog surface is the full 2,089).
Pack entry count == catalog count == manifest count + 20,988.

## 9. Staging / shadow parity (brief §18–§23, §30)

`h2_stage.py` runs the full gate ladder at two host bases
(`gen-a 0x10000000`, `gen-b 0x50000000`):

- §20 containment round-trip over all 90,047 bytes; hole refusals; stale /
  cross-base tokens refuse; gba containment base-independent;
- §21 host word == final_gba at all 7,549 sites at both bases; words
  identical across bases; raw == addend;
- §22 class counts; 718 binding rows unique; tables A–F 50/50/327/213/11/67;
  every engine target bound;
- §23 canonical aggregate hash identical at both bases, host pointers
  differ by exactly 0x40000000 (delta = base delta);
- §30 **21/21 fault-matrix refusals** (schemas, lengths, digests,
  duplicates, boundaries, sources, words, targets, addends, nulls, layout),
  each with a distinct mutation, all refused; the prior generation is
  preserved after every fault (token + canonical hash unchanged);

**Result: H2 stage: 54 pass / 0 fail** (gen-a, gen-b, pack 23,069).

## 10. Loader test (brief §25)

`tests/emerald_battle_module_loader_test.c` +
`tests/run_emerald_battle_module_loader.sh` (mirrors the G2 script-module
loader harness): **8,346 checks, 0 failures** — pack identity (23,069
entries, ROM SHA-1 f3ae0881…), all 2,081 modules found by canonical name
and byte-exact vs committed artifacts with both manifest digests matching,
the 8 zero-width aliases absent from the pack, no pack battle entry
outside the 2,089-entry catalog, and the tamper triad (payload flip →
PAYLOAD_HASH_MISMATCH, TOC flip → refused, truncation → refused).
Ownership gate: 2,089/2,089 COMPILED_PENDING_MIGRATION; family counts
645/658/553/165/68 = 2,089, schemas 47/48/49/50/51.

## 11. No production execution change (brief §19, §28, §29)

- Zero runtime source changes: `git diff src/ include/` is empty; the
  native binaries link no battle-module seam (nothing consumed them).
- No State-v5 range registered; the state suite pins **6,377 ranges, 0 H
  ranges** unchanged.
- The 12 leaf-test scripts that pin the pack surface were updated
  20,988 → 23,069; the three provider harnesses (trainer production,
  native world real, native world render proof) now pass the 2,081-entry
  embedded battle catalog so the session construction catalog exactly
  matches the pack's provider surface; the two leaf runners' pack-rebuild
  invocations were re-ordered to the canonical build_d1_pack.sh order
  (the pack profile's provenance digest covers manifests in input order).

## 12. Regression battery (brief §31, §32)

| Test | Result |
|---|---:|
| script module loader | 1,938 checks, 0 failures |
| **battle module loader (new)** | **8,346 checks, 0 failures** |
| script compat | passed (incl. isolation sweep clean) |
| runtime loader | 65,754 checks |
| trainer native compat production | 37,921 checks |
| native world real | 5,570 checks |
| native world render proof | 3,631 checks |
| R13-B leaf runner | 7/7 |
| audio leaf runner | 10,375 checks, 3/3 |
| script state (State-v5) | G4 5 capture + 5 restore cases, 2 arena generations |
| script faults | 21/21 |
| script state faults | 31/31 capture/restore/identity checks |
| script state cross-restart | passed (5 capture + 5 restore, 2 arena generations) |
| native asset isolation | PASS (5,819 ROM_BASE_ONLY / 1,057 CPM scoped) |
| native world neighborhood | 319 checks, 0 failures |
| H2 generator --check | byte-identical, 2,089/2,081/90,047/7,549/65/3,058/2,109/718 |
| pack determinism (build_d1_pack.sh --check) | byte-identical to the deterministic rebuild |

**Forced builds (§32):** `make -f Makefile_pc -B linux64` (release, -O3,
0 debug sections, 23,307,480 B) and `make -f Makefile_pc -B linux64
DINFO=1` (-O0 -g, 36,021,856 B) — both
`--verify-game-data` → **"Pokemon Emerald content verified:
f3ae088181bf583e55daf962a92bb46f4f1d07b7"**, exit 0.

## 13. H3 readiness artifacts (brief §33)

All inputs H3 needs are committed as generated artifacts: per-module
boundary maps, per-family arena spans (5 arenas + 2 holes, 8,192-cap
arithmetic unchanged), the reverse-containment index, the 718-row binding
table, the source relocation index, the target binding index, and the
65 NULL-sentinel list. The staging harness already exercises
forced-different-base placement and generation-preserving refusals —
the exact mechanics H3's dry-run gates require.

## 14. STOP conditions (brief §36, §37)

All met. Nothing committed; H3 not begun; R13-I not begun; the working
tree carries only additive resources/tests/tools/report changes.

## Final pins

- Modules 2,089 · payload records 2,081 · canonical bytes 90,047 ·
  relocs 7,549 · NULL sentinels 65 · nonzero addends 3,058 ·
  exports 2,109 names / 20 aliases · bindings 718 (A–F 50/50/327/213/11/67)
  · SCRIPT_TARGET 3,798 root / interior 0 · ENGINE 3,751 ·
  pack entries **23,069** · pack 15,278,272 B · SHA-256
  `b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb` ·
  live State-v5 ranges 6,377 (0 H) · ROM SHA-1
  `f3ae088181bf583e55daf962a92bb46f4f1d07b7` ·
  ownership 2,089 × COMPILED_PENDING_MIGRATION ·
  loader test 8,346/0 · stage harness 54/0 · fault matrix 21/21.
