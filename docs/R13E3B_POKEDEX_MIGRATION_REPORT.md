# R13-E3b — Pokédex Structured-Data Migration + R13-C Text Cutover Report

Stage scope: migrate `gPokedexEntries` (387) + the Pokédex ordering/routing
tables to ROM-owned resources and complete the R13-C Pokédex **description**
text cutover via a new `EmeraldPokedexCompat` seam. **No commit. R13-F not
started.** Dex area/region graphics, map metadata, scripts, and unrelated
deferred text families are NOT in E3b.

Status flags: ✅ done & verified.

> Implementation note: the delegated implementation agent was terminated by a
> content-filter false positive near the end; it had completed the generator,
> pack, seam, loader wiring, guards, and the 387-label text flip but left two
> stale seam `#error` pins (6,517) and three compile defects in
> `emerald_pokedex_compat.c`. I diagnosed and fixed those (pins → 6,908; the
> missing `emerald_resource_session.h` include; the const `whole[]` array with
> a non-constant initializer), wired the pokedex TUs into the loader test
> runner, rebuilt, and verified all gates.

---

## 1. Exact inventory + layout correction (verified against the qualified ROM)

**Inventory correction:** the review baseline's "40 B / 15,480 / 2 text
pointers" is a native-size/pointer-count conflation. The ROM is authoritative:

- `gPokedexEntries`: **387 rows × 32 B GBA wire = 12,384 B** (@0x56b5b0);
  native 40 B. Row: `categoryName[12]` **inline** Gen-3 string @0 (not a
  pointer) + height u16 @12 + weight u16 @14 + **description u32 ptr @16**
  (the ONLY text pointer) + unused + pokemonScale/Offset + trainerScale/Offset.
- `sSpeciesToNationalPokedexNum`: 411 u16 = 822 B.
- `gPokedexOrder_Alphabetical`: 411 u16 = 822 B.
- `gPokedexOrder_Height`: 386 u16 = 772 B.
- `gPokedexOrder_Weight`: 386 u16 = 772 B.
- R13-C Pokédex text: **387 description labels** (386 species +
  gdummypokedextext), 58,017 B, currently COMPILED_PENDING_MIGRATION → flip
  ROM_BASE_ONLY. Category is inline (no separate text resource).

## 2. Layout correction vs the task brief

| Field | Brief baseline | ROM truth |
|---|---|---|
| GBA wire row | 40 B | **32 B** |
| canonical total | 15,480 B | **12,384 B** |
| text pointers/row | 2 (category + description) | **1 (description)**; category inline |

## 3. Resource identity

```
emerald:data/pokedex/<species>              (387)
emerald:data/pokedex/order/alphabetical     (822 B)
emerald:data/pokedex/order/height            (772 B)
emerald:data/pokedex/order/weight            (772 B)
emerald:data/pokedex/species-to-national     (822 B)
```
No structured category/description text resources (R13-C identities retained).

## 4. Parity gate

All 387 entries + routing/order tables compared recomp-vs-ROM (full): only
layout (32→40 native) + pointer-width differences; description pointer
identity matches symbol-for-symbol; oredering u16 content identical. No C/D
divergence (any unexplained D-class = STOP).

## 5. Publication (EmeraldPokedexCompat)

Transactional phase-1 validates every row (schema, 32 B, ROM_BASE winner) +
every description pointer → R13-C text identity (387/387) + the ordering/
routing resources + parity/oracle. Phase-2/3 publishes native `gPokedexEntries`
(387 × 40 B: category inline + scalars byte-identical, description re-pointed
into the R13-C text arena) + the native ordering arrays. Atomic, REFUSE-class.

## 6. R13-C text cutover

387 pokedex description labels flip COMPILED_PENDING_MIGRATION → **ROM_BASE_ONLY**
(58,017 B); compiled native s<Species>PokedexText payloads guarded out once
gPokedexEntries uses the published arena pointers. No unrelated deferred
families touched (easy-chat/match-call/berry/decor/TV/landmark stay).

## 7. State-v5

TextPrinter.currentChar inside the R13-C Pokédex-text arena is already handled
by R13-C; a Pokédex structured arena range is registered only if a serialized
pointer surface is proven (expected none). Post-E3b range index = **5,851 /
8,192**. No format change, no cap raise.

## 8. Regression battery + builds ✅

**Builds:** `make -f Makefile_pc linux64` links (exit 0); `--verify-game-data`
exits 0 over the 18,971-entry pack (the full seam chain incl. pokedex).

**Battery (all green), 18,971-entry pack:** loader **65,658**; trainer **37,921**;
world **5,565**; render **3,628**.

**Pack:** 18,580 → **18,971 entries**, 13,987,040 B, SHA-256
`4e2be72806ff1947024658f01c4018064a194e99570aef2ee48d325a7d08b72e`,
deterministic. Cap unchanged (32,768).

**Binary (release):** E3a-2 22,488,384 → E3b **22,442,296 B (−46,088 B)** (net
shrink: removed compiled gPokedexEntries 15,480 + pokedex description text
~58,017 vs the seam/generated metadata). (A separate DINFO-flavor delta was
not captured.)

**Isolation (binary-level):** `gPokedexEntries` (15,480 = 387 × 40 host_data),
`gPokedexOrder_Alphabetical/Height/Weight` + `sSpeciesToNationalPokedexNum`
are host_data fill targets; compiled pokedex description payloads absent.
**Text flip:** 387 pokedex description labels → ROM_BASE_ONLY (text ownership
3,816 ROM_BASE / 1,371 pending; pokedex 387/387 RB, 0 pending); easy-chat /
match-call / berry / decor / TV / landmark stay deferred.

Focused E3b gates verified: exact inventory, full parity, generator `--check`
(6,908 / 509,975 B), 387/387 description→text bindings, gPokedexEntries
publication, ordering/routing proof, text ownership flip, State-v5 text
relocation (R13-C arena; no new structured range needed), isolation, failure
matrix.

## 9. Manual gate (DINFO checklist)

1. open the Pokédex; 2. inspect several entries; 3. category text correct;
4. description text correct; 5. height/weight correct; 6. alphabetical sort;
7. height sort; 8. weight sort; 9. National-dex mapping; 10. save/load while
viewing/printing an entry if practical; 11. no garbled text/wrong species/
crash.

## 10. Blockers / prerequisites for R13-F

- E3b completes the E3 (Frontier + Pokédex) scope. R13-F (map metadata) is
  independent; the pack cap (32,768) has ample headroom. No E3b debt blocks
  R13-F. Remaining deferred: dex area/region graphics (later gfx stage).

**STOP — R13-E3b complete. No commit made. R13-F not started.**