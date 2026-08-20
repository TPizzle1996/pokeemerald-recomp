# R13-D2 — gItems Transformed Publication + Callback Registry + Item-Description Cutover Report

Stage scope: migrate `gItems` (377 rows) to ROM-owned canonical resources,
publish the native 72 B rows via the R13-D1 gameplay seam, add a 27-entry
item-use callback registry, and complete the R13-C-deferred item-description
text cutover. **No commit. R13-E not started.** gItemEffectTable / item icons
are out of scope (stay compiled). Item count is not expanded.

Status flags: ✅ done & verified · 🅿 pending (none — all gates recorded below).

---

## 1. Item inventory (pinned, verified)

| Quantity | Value |
|---|---|
| `gItems` canonical resources | **377** (`emerald:data/item/<name>`, schema 11, STRUCTURED_DATA, gba-bytes) |
| Canonical GBA row | **44 B** |
| Canonical total | **377 × 44 = 16,588 B** (ROM `gItems` @ 0x085839A0, 16,588 B — verified) |
| Native row | **72 B** `struct Item`, layout unchanged |
| Native published table | **377 × 72 = 27,144 B** |
| Item enum coverage | indices 0..376 fully named (ITEMS_COUNT=377); 67 empty rows are `itemId=0` placeholders (identical vanilla/recomp) |
| R13-C item-description resources | **310** (`emerald:text/item/s<item>desc`), previously COMPILED_PENDING_MIGRATION |
| Callback registry | **27** distinct (21 field-use + 6 battle-use) |

Item resources + callback registry are emitted deterministically by
`tools/gen3_resources/gameplay_family/gen_gameplay_family.py` (`--check`
byte-identical no-op). Gameplay total with D1 = 3,687 resources / 358,818 B.

## 2. Compile-vs-vanilla parity gate (mandatory pre-cutover)

Field-by-field semantic comparison of all 377 recomp-compiled native rows vs
the vanilla 44 B ROM rows (name, itemId, price, holdEffect, holdEffectParam,
importance, registrability, pocket, type, battleUsage, secondaryId; plus
description → label identity and both callbacks → function identity via the
symbol tables on both sides).

**Verdict: DIVERGE — 6 / 377 rows (1.6%), otherwise PASS.** The feared
newer-pret price/hold drift is absent: 371/371 other data rows, **all 27
callbacks**, and **all 310 descriptions** match vanilla exactly.

The 6 divergent rows are the **trade-evolution held items**, each differing in
exactly two fields (`type` + `fieldUseFunc`):

| id | item | vanilla | recomp |
|---|---|---|---|
| 187 | King's Rock | type `0x04` (BAG_MENU), `CannotUse` | type `0x01` (PARTY_MENU), `EvolutionStone` |
| 192 | Deep Sea Tooth | `0x04`, `CannotUse` | `0x01`, `EvolutionStone` |
| 193 | Deep Sea Scale | `0x04`, `CannotUse` | `0x01`, `EvolutionStone` |
| 199 | Metal Coat | `0x04`, `CannotUse` | `0x01`, `EvolutionStone` |
| 201 | Dragon Scale | `0x04`, `CannotUse` | `0x01`, `EvolutionStone` |
| 218 | Up-Grade | `0x04`, `CannotUse` | `0x01`, `EvolutionStone` |

Root cause: a **deliberate source-level QOL change** in `src/data/items.h`
("make trade-evolution held items directly usable to trigger evolution") — the
same intent as the D1 evolution-family finding. Everything else is identical.

**Handling (NOT silently normalized):** the canonical pack rows are vanilla;
the publication seam applies an **explicit, machine-checked 6-row override**
(`type = PARTY_MENU`, `fieldUseFunc = ItemUseOutOfBattle_EvolutionStone`) for
those ids, guarded so each override is only applied when the vanilla row is
the expected `type==0x04 && CannotUse` baseline (else REFUSE). The oracle
equality (§4) runs against the overridden form → **377/377**.

## 3. Callback registry (27-entry census)

Generated from the reference ELF: every distinct non-zero GBA use-function
address across the 377 rows → `{gbaAddr, action, symbol}`
(`gameplay_callbacks.generated.{h,c}` + `item_callbacks.generated.toml`).
Semantic ids are stable tooling-friendly (`item-use/out-of-battle/medicine`,
`item-use/in-battle/pokeball`, …). The pack resource never stores a native
function pointer — it holds the exact 44 B ROM row (GBA addresses included);
the seam resolves each address through the registry to a native `ItemUseFunc`
via a compiled engine table (ENGINE_CONSTANT metadata).

|| count | names |
|---|---|---|
| field-use (21) | 213 CannotUse · 58 TMHM · 38 Medicine · 12 EvolutionStone · 12 Mail · 6 ReduceEV · 5 PPRecovery · 3 Repel · 3 Rod · 2 BlackWhiteFlute · 2 PPUp · 2 Bike · 1 SacredAsh · 1 RareCandy · 1 EscapeRope · 1 EnigmaBerry · 1 CoinCase · 1 Itemfinder · 1 WailmerPail · 1 PokeblockCase · 1 PowderJar |
| battle-use (6) | 35 InBattle Medicine · 12 PokeBall · 7 StatIncrease · 5 PPRecovery · 2 Escape · 1 InBattle EnigmaBerry |

Description census: 310 distinct symbols, 0 NULL, 309 unique + `sDummyDesc`
shared by 68 dummy rows — all bound by symbol identity (recomp==vanilla).

## 4. Oracle equality (pre-cutover) ✅

Before guarding out the compiled `gItems`, the parity gate compared the
transformed-overridden migrated rows against the existing compiled native rows
field by field (name bytes, all scalars, description target identity,
field/battle callback identity) for all 377 rows. Result: **377/377 semantic
rows, 0 mismatches** — the 6-override reproduction makes the migrated table
semantically identical to today's compiled table; the other 371 rows match
vanilla directly. (Pointer addresses differ by design: descriptions re-point
to the text arena, callbacks to the registry-resolved native functions, but
the semantic targets match.)

## 5. Publication + cutover ✅

- Native `HOST_DATA struct Item gItems[ITEMS_COUNT]` filled by
  `EmeraldGameplayCompat` (D2 item phase); the compiled `gItems` in
  `src/data/items.h` is guarded out on native (GBA verbatim).
- The 44→72 B transform: copy canonical data fields; re-point `description`
  into the R13-C item text arena; resolve both callback GBA addresses through
  the registry → native functions; apply the guarded 6-row override.
- `gItems` consumers (the ~15 `item.c` accessors + the rest) are unchanged —
  zero mass call-site edits. Verified: `gItems` is now section 28 (host_data,
  27,144 B fill target, single definition on native); the compiled const
  `gItems` and all `s*Desc` are absent from the binary.

## 6. Item-description cutover ✅

- The 310 `emerald:text/item/*` resources flipped to **ROM_BASE_ONLY**;
  text ownership ROM_BASE_ONLY 3119 → **3429**, COMPILED_PENDING 2068 →
  **1758** (exactly +310/−310). Pokédex (387) and easy-chat stay
  COMPILED_PENDING_MIGRATION (not touched).
- The compiled native `s*Desc` payloads are guarded out (`src/data/text/
  item_descriptions.h`, `#ifndef NATIVE_LINUX`); `gItems[].description`
  resolves into the R13-C item text arena. Dual-reference audit clean: the
  only native consumer of the item `s*Desc` symbols is `gItems[].description`
  (they are `static` to the item.c TU); no still-live compiled consumer is
  removed.

## 7. State-v5 ✅

No format change; no new sidecar class; **no per-item ranges**. The live
pointer scenario (TextPrinter.currentChar → item-description text arena) is
already covered by R13-C's arena/currentChar routing. Range count stays
**5,847** (D1); the D2 `gItems` fill target is fixed host_data .data (no range).

## 8. Ownership totals / pack / ranges ✅

- Pack: 15,373 → **15,750 entries**, 13,190,320 B, SHA-256
  `71a0ba0e605c82797eb8eb2cc4af98491c966944cfd3689a286c073837b95291`,
  deterministic rebuild byte-identical (headroom 634; no cap raise).
- Ownership: +377 item resources and +310 item-description flips to
  ROM_BASE_ONLY. Pack entries grow by 377 (the 310 descriptions already
  existed in the pack).

## 9. Regression battery + builds ✅

**Builds:** `make -f Makefile_pc linux64` links (exit 0); `--verify-game-data`
exits 0 over the 15,750-entry pack (full seam chain incl. D2 gItems
publication).

**Battery (all green):**

| Suite | Checks | Result |
|---|---|---|
| `run_emerald_runtime_loader.sh` | 62,945 | ✅ PASS (incl. D2 gItems checks: potion price 300 + field/battle medicine callbacks; master-ball battle callback; King's Rock override `type=PARTY_MENU` + battle-NULL; potion name byte 0xCA; potion description in/equals `emerald:text/item/spotiondesc` bytes; dummy rows bound to `sdummydesc`) |
| `run_emerald_trainer_native_compat_production.sh` | 37,921 | ✅ PASS (15,750-pack) |
| `run_emerald_native_world_real.sh` | 5,565 | ✅ PASS |
| `run_emerald_native_world_render_proof.sh` | 3,628 | ✅ PASS |

**Binary deltas (release):** R13-D1 22,368,088 B → R13-D2 **22,363,584 B** =
**−4,504 B**. Removed compiled `gItems` (27,144 B from .rodata) +
item-description text (~15,101 B) net of the D2 seam + 27-entry registry +
`gItems` HOST_DATA fill target. (A separate DINFO-flavor delta was not
captured in this pass.)

**Isolation (binary-level):** `gItems` in section 28 (host_data fill target,
single native definition); compiled const `gItems` + all `s*Desc` symbols
absent from the binary.

Focused D2 gates verified: item generator `--check` (byte-identical), 377/377
provenance + oracle, 27/27 registry, 310/310 text binding, gItems residency,
item-description currentChar relocation, failure paths (unknown callback /
description address, override guard, missing item row → REFUSE via the seam's
new error codes).

## 10. Manual gate (DINFO checklist)

1. Open the Bag; 2. inspect several item names + descriptions; 3. use
Potion/medicine in the overworld; 4. use TM/HM; 5. use an evolution stone
(and King's Rock / Metal Coat / etc. — the recomp's usable-held-item feature);
6. use Repel; 7. Bike / Rod / key-item path; 8. battle: medicine; 9. battle:
throw a Poké Ball; 10. save/load while an item description is printing;
11. no wrong text / callback / crash.

## 11. Blockers / prerequisites for R13-E

- R13-E (trainer/encounter/frontier/pokedex) is unaffected by D2; the pack
  cap at 15,750/16,384 gives 634 headroom but R13-E's ~3,500 resources
  require a cap raise then (documented).
- No D2 debt blocks R13-E.

**STOP — R13-D2 complete. No commit made. R13-E not started.**