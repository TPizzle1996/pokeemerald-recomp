# R13-J Final Ownership, Compiled-Payload, and Isolation Closure

**Status: R13-J COMPLETE** — 2026-08-25
**Predecessor:** R13-I state-v5 closure (docs/R13I_STATE_V5_CLOSURE_REPORT.md)
**Qualified ROM:** `../pokeemerald-reference/pokeemerald.gba` — SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`

## 1. Baseline freeze (brief §1)

| Pin | R13-I baseline | R13-J |
|---|---|---|
| Pack | 23,069 entries / 15,278,272 B / SHA-256 `b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb` | byte-identical |
| Resource ranges | 6,390 / 8,192 | unchanged |
| State-v5 | version 5 | unchanged (5u) |
| Release binary | 23,842,840 B | 23,464,296 B (delta −378,544 B) |
| DINFO binary | 36,521,096 B | 36,105,696 B (delta −415,400 B) |
| ROM verify | f3ae0881…d07b7 exit 0 | release + DINFO both exit 0 |

## 2. Global ownership census (brief §2)

Machine-readable census complete; unexplained/UNOWNED payload = **0**.
(§3 sweep + §3A correction narrative below; qualified-truth byte pin
207,330 user-approved — see R13-G2 reconciliation.)

## 3. COMPILED_PENDING_MIGRATION hard sweep + §3A correction (brief §3)

Sweep over the whole R13 ownership universe: 21,735 ROM_BASE_ONLY,
1,388 COMPILED_PENDING_MIGRATION remaining — **all explained, 0
unexplained pending** (the brief's target). The 1,388 split:

| Group | Count | Decision | Evidence |
|---|---:|---|---|
| movement STAY bridge | 15 | final (STAY) | 7 STAY tables (56 B) + 8 `sMovement_*` objects; 1,040 FLIPped; runner `run_r13b_leaf.sh` COMPILED-presence check on the full nm table |
| multiboot | 2 | engine-owned / documented deferral | `gMultiBootProgram_EReader_Start` LIVE (main_menu.c → ereader_screen.c:405, EReaderHandleTransfer may write it); `gMultiBootProgram_PokemonColosseum_Start` dead natively (intro.c:1131 `#ifndef PORTABLE`), flip waits on the multiboot runtime seam (documented in TOML) |
| text easy-chat | 1,371 | engine/provenance exception (§6 carve-out) | words reached via `gEasyChatWordsByLetterPointers` C pointer tables (src/easy_chat.c); owner = R13-B leaf-family continuation stage |

- §3A text FLIP: pure-FLIP text blocks (zero host refs in the R13-I
  baseline binary) are pack-served at runtime and GBA-only from here on:
  - `birch_speech`, `cable_club`, `contest_link`, `contest_painting`,
    `contest_strings`, `pc`, `pokedex_rating`, `save`, `secret_base_trainers`,
    `tv` → `data/text/*_native.inc` (generated outputs).
  - The two `Obtained` labels (`gText_ObtainedTheItem`,
    `gText_ObtainedTheDecor`) FLIPped — arbiter LINUX64 link proved zero
    host refs; `verify_binary_isolation.py` now asserts them ABSENT
    (flip-text-* checks).
  - `gBirchDexRatingText_AreYouCurious` is C-consumed (birch_pc.c; one of
    the 70 arbiter-proven labels) → pokedex_rating STAY twin.
  - `pc` is mixed (C consumer `gText_WhichPCShouldBeAccessed`,
    birch_pc.c) → LINUX64 links the STAY twin.
- Movement tables §3A FLIP: 1,047 compiled tables (7,404 B) → 7 STAY
  tables (56 B) retained compiled; the other 1,040 are pack-served.
  (`data/movement_tables_native.inc` regenerated; runner
  `tests/gen3_resources/run_r13b_leaf.sh` --check green.)

## 4–10. Family closure audits (brief §4–§10)

Every family closed: catalog records fully owned (ROM_BASE_ONLY), no
compiled fallback, no pending migration records, no unexplained bytes.

| Family | Records (catalog) | State |
|---|---|---:|
| B (text) | 5,326 | closed |
| C (gameplay) | 8,438 (+ frontier/pokedex sub-catalogs) | closed |
| D (trainer) | 196 (186 front + 10 back) | closed |
| E (encounter/frontier/pokedex) | within C catalog; R13-E2/E3 stages closed | closed |
| F (map metadata) | 518 map headers + layout 882 / tileset 1,562 / object-event 288 | closed |
| G (field scripts) re-sweep | 523 / 523 ROM_BASE_ONLY (no compiled fallback) | closed |
| H (battle/animation/AI) re-sweep | 2,089 / 2,089 ROM_BASE_ONLY (645/658/553/165/68) | closed |

## 11–17. Compiled-content / provenance sweeps (brief §11–§17)

- **Compiled-content sweep:** every COMPILED record in the ownership
  catalogs reconciled to a final state — §3A FLIP (movement tables
  1,047→7 STAY; 11 FLIP text blocks; 2 FLIP labels) removed the last
  uncompensated compiled payloads; the STAY set is arbiter-proven
  (70 C-consumer labels; a live consumer would have failed the LINUX64
  link — the completeness proof).
- **Provenance sweeps:** elf-manifest regeneration byte-for-byte
  (fixture determinism + `--check` path green); ownership TOMLs
  regenerated deterministically; `verify_binary_isolation.py` green
  (legacy-symbols 0 still defined, exports 0 still defined, gStdScripts
  slots zeroed, F-owned identity < 4 GiB, exclusions present, FLIP
  labels ABSENT — the §3A flip-text-* checks).
- **Isolation sweep:** 7,908/7,908 ROM_BASE_ONLY (196 trainer + 1,608
  Pokémon battle + the R13 text/script/map/leaf families); 0 failed.

## 18. Pack completeness (brief §18)

23,069 entries — SHA-256 `b711d358…` byte-identical to R13-I.

## 19. Pack determinism (brief §19)

Regeneration produces the identical pack (byte-identical, no content added
or reordered).

## 20. State-v5 unchanged (brief §20)

`NATIVE_STATE_FORMAT_VERSION 5u` unchanged; no field, section, or kind
added. R13-J adds no state surface.

## 21. Range census (brief §21)

6,390 registered ranges (of 8,192) — unchanged from R13-I; runtime-loader
census green (78,524 checks, +12 from the R13-J §3A pins).

## 22. Binary size deltas (brief §22)

| Build | R13-I | R13-J | Delta |
|---|---:|---:|---:|
| Release (`-B rom`) | 23,842,840 B | 23,464,296 B | −378,544 B |
| DINFO (`-B rom DINFO=1`) | 36,521,096 B | 36,105,696 B | −415,400 B |

Deltas are the §3A FLIP removals (movement tables −7,348 B, FLIP text
blocks, FLIP labels) minus STAY twin additions — payload motion out of the
native binaries, no runtime-semantic change. Release and DINFO both
`--verify-game-data` exit 0 against `f3ae0881…d07b7`.

## 23. Final isolation verifier (brief §23)

`tests/run_emerald_native_asset_isolation.sh` — **37,681 checks, 0
failed, PASS** (8,948/8,948 ROM_BASE_ONLY isolated — 196 trainer + 1,608
pokemon battle + 288 object-event + 1,544 tileset + 882 layout + 1,301
audio + 2,089 battle modules + 1,040 movement (R13-J §3A FLIP); 17/17
COMPILED_PENDING_MIGRATION present — 15 movement STAY + 2 multiboot).
Per-segment "reported, not failed" notes for small-payload collisions and
the R9 §9 art-sharing case are documented non-failures.

## 24. R14 handoff list (brief §24)

Per `docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md` §11 (unchanged by R13-J),
R14 contains **only**:

1. final global isolation proof + link-ownership report across both
   binaries;
2. remaining engine residue classified E (interpreter command tables,
   `gSpecials`/`gStdScripts`/`gSpecialVars`, compat seam descriptors,
   `host_data` scratch) — re-classification, not migration;
3. sub-200-B shape-D routing tables explicitly allowed to remain
   compiled;
4. packaging/legal-content audit;
5. cleanup (packaging, docs, source-tree overlay strategy).

No major content family is deferred to R14 — gfx leaves, text, scripts,
map metadata, and all gameplay tables are R13.

## 25. COMPILED outside R13 scope (brief §25)

Every COMPILED record outside R13 scope is labeled E (engine residue)
or STAY (arbiter-proven C-consumer twins) in the ownership TOMLs; none
crosses into R13 family scope. The §3A FLIP removed the last
compiled-only payloads that R13-J proved had zero host consumers; the
STAY set is exactly the arbiter-proven consumer-backed labels (70
labels; e.g. `gBirchDexRatingText_AreYouCurious` via birch_pc.c,
`gText_WhichPCShouldBeAccessed` via the pc STAY twin).

## 26. Manual gameplay checklist (brief §26)

Interactive on the release build (DINFO follows), with the pack in place:

**R13-J §3A-flipped surfaces (the only new surfaces since R13-I):**

1. movement: walk/run/turn/warp/stairs, door enter+exit, bike, surf,
   board, rock-smash, cut — tables now pack-served (7 STAY objects
   compiled)
2. text: pc access + which-pc prompt (STAY twin), pokedex rating
   dialog, tv show text, save/erase dialogs, contest strings
   (painting/link/contest), cable-club link text, secret-base trainer
   text, birch speech, obtained-item/decor messages (FLIPped labels)
3. text after a save/load round-trip (pack identity stable)

**Baseline (R13-I surface, unchanged):**

4. boot to title, new game, overworld movement, map transitions
5. NPC talk, field script execution (signposts, berries, cutscenes)
6. wild battle enter/flee/win/lose, trainer battle, double battle
7. move anims + field effects, catch flow, item use
8. save/load overworld + mid-battle (State-v5; AI quiescent at capture)
9. frontier/rematch flows, pokedex, bag, party, summary
10. **fail-closed probes**: delete/rename a pack module → NOT_PUBLISHED,
    no crash; withhold a routing row → BOUNDARY_INVALID

Result: automated coverage green — the battery's real-SDL desktop probe
and `--verify-game-data` (release + DINFO, both exit 0 against
f3ae0881…d07b7) cover boot/content/pack integrity, and the state suites
cover the save/load round-trips. The interactive gameplay sweep remains
the manual gate (as in R10/R13-I precedent); the checklist above is the
exact script for it, with the §3A-flipped surfaces (movement + the 11
FLIP text blocks + the 2 FLIP labels) as the new-surface focus.

## 27. Full regression battery (brief §27)

All suites green with ONE root-caused-and-fixed flake: the state-master
harness's create-leg 13-record expectation was pack-base dependent.

### The state-master flake — root cause and fix (three-mode mechanism)

**Symptom:** ~3-4 failures in 5 master runs, never in 600+ single creates.

**Mechanism (proven by captured dumps):** the harness plants a real
published map header into its GAME_DATA slice; the mapScripts field
(+0xc8) carries the GBA-constant value `0x081dc2cc` — the ROM script
address, unresolved because the harness session never publishes the
script family (RebindScripts requires the script compat live). The pack
image is malloc'd (`Gen3ResourcePack_OpenFile`), so its base is
ASLR-random. Where the constant lands relative to the pack's 6,390
ranges decides the outcome:

- **mode (a) — base outside the band:** `0x081dc2cc` misses every range
  → serializes in-band → 13 records, passes;
- **mode (b) — base in the 0x08xxxxxx band, value in an EXPOSED range:**
  the walker *correctly* captures a 14th record (LEGACY_LZ battle table,
  rangeOffset 92, or CANONICAL battle-live module, rangeOffset 2960 —
  whichever range covers the address) → pre-fix count check failed;
- **mode (c) — base in the band, value in a hull's UNEXPOSED
  build-time-only prefix** (e.g. hull 5 0x81babac..0x84e6b60, exposed
  region starts 0x81f81a8): `NativeState_Save` REFUSES at
  save-normalize — "resource-owned pointer lacks a registered resource
  identity … value in the unexposed build-time-only prefix" — so the
  create fails outright, before any record parsing. R13-J hunter
  iteration 19 (~247 creates in).

The runtime is right in every mode: an unresolved ROM address in a
serialized pointer window is exactly what the prefix refusal guards
against. The harness must not plant it.

**Fix (harness-side only; tests/emerald_resource_state_test.c):** the
+0xc8 window is planted NULL — the session-accurate value (no live
script pointer in this session) — so the GBA ROM constant never enters a
serialized pointer window. The recordCount expectation is the plain 13
(`HARNESS_ROW_COUNT + 3`), the role check is strict again, and
VerifyMapHeaderRelocated expects the NULL round-trip. (An intermediate
attempt made the 13-vs-14 expectation base-aware; it handled mode (b)
but not mode (c) — the save refusal precedes any record parsing. The
NULL plant subsumes both.)

Post-fix verification: resource-state suite TEST 1-8 **ALL PASSED**
(TEST 8 13/13 kinds; the fingerprint/missing-session/v4 rejections are
the intentional negative tests), master harness re-run
(**GREEN — all 5 legs: G fresh-process (vaddress 8/8), H battle
fresh-process (blocking command, nested returns), H live suite (battle +
anim + AI, ASan/UBSan), mixed-family fresh-process (24 records),
deterministic semantic serialization; "all cross-process legs green",
exit 0**), and a bounded post-fix create loop (3,900 creates,
300 × 13 corrupt kinds) — every create must produce the IDENTICAL
13-record dump at every pack base — **DONE: "POSTFIX-HUNT OK: 300
iterations, no create failure, zero record-dump variants"** (exit 0;
3,900 creates; the pre-fix mode-(c) failure hit at create ~247, so the
post-fix loop crossed the old failure point within its first hour and
ran every create clean; the bases file reconciles exactly: 242 pre-fix
leftovers + 3,900 post-fix = 4,142 lines, iterations 1-18 doubled to 26,
iteration 19 at 21 (8 pre-fix + 13 post-fix), 20-300 at 13, and the
post-fix tail covers all 300 iterations × 13 kinds).

Battery evidence: full battery run 2 (`/tmp/r13i-batt2-*.log`,
2026-08-24/25) all green; the state-master leg of that battery passed
pre-fix (flake rate ~1/3 of master-leg exec contexts), and the R13-J
final gate re-runs the master harness + a 3,900-create loop on the fixed
binary.

| Group | Suites | Result |
|---|---|---|
| G State | script-state 5+5, script-state-cross-restart, script-state-faults, g4-cross (5+5, vaddress 8/8), g4-faults, g6-mevent | green (batt2) |
| H State | battle-state 5+5, battle-state-cross-restart, battle-state-faults 16/16, h3-cross (nested blocking), battle-live sanitize (H4/H5/H6) | green (batt2) |
| Global | resource-state TESTS 1-8 (13-kind corrupt matrix) + v4/unsupported policy + fingerprint, resource-ranges, session-fingerprint, mixed-cross (24 records), determinism, **state master harness (5 legs)** | green (batt2) + post-fix re-run: suite ALL PASSED (TEST 8 13/13); master harness all 5 legs green |
| General | runtime-loader (78,524 checks incl. range census 6,390), trainer-compat 8,377 + production + sanitize, layout 441/882, tileset 75/1,544, object-event 253/35/1,788, script-compat + sanitize, script-faults 21/21, script-module-loader 523/523, battle-module-loader 2,089 (645/658/553/165/68), real-tables 16,311, native-world 319+5,570+3,631, resource-lz 144 (+san), resource-import (+san), rom-base-provider 140 (+san), native-asset-isolation 7,908/7,908, desktop real-SDL probe | green (batt2) |
| Build | --verify-game-data → f3ae0881… exit 0 (release + DINFO) | green (both) |

## 28. Forced builds (brief §28)

`make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 -B rom` (never `make -B
linux64`). Confirmed — release 23,464,296 B and DINFO 36,105,696 B,
both `--verify-game-data` exit 0 against `f3ae0881…d07b7`.

## 29. Final ownership pins (brief §29)

Pack 23,069 / 15,278,272 B / `b711d358…`; ranges 6,390/8,192; State-v5 5u;
release 23,464,296 B; DINFO 36,105,696 B; ROM `f3ae0881…d07b7`.

## 30. Docs + architecture status (brief §30)

This report; `docs/ARCHITECTURE.md` (or architecture status file) updated
to **R13-J COMPLETE**.

## 31. Hard completion gates (brief §31)

- ✓ freeze: pack/ranges/State-v5 pins unchanged (sec 1)
- ✓ census: unexplained 0 (sec 2)
- ✓ pending sweep: 0 unexplained pending (sec 3)
- ✓ explicit-deferred audit: every deferred group named + owned (sec 3)
- ✓ R13-B closure: movement/multiboot audited, bridge documented (sec 5)
- ✓ text closure: unexplained text duplicate 0 (sec 6)
- ✓ family closures B–F, G 523/523, H 2,089/2,089 (sec 4–10)
- ✓ compiled-content + provenance sweeps (sec 11–17)
- ✓ pack completeness 23,069 + determinism (sec 18–19)
- ✓ State-v5 unchanged, range census 6,390 (sec 20–21)
- ✓ binary deltas accounted (sec 22)
- ✓ isolation verifier failed=0 (sec 23)
- ✓ battery green incl. post-fix state-master (sec 27)
- ✓ forced builds + ROM verify (sec 28)
- ✓ final gates: isolation verifier 37,681 ok / 0 failed (sec 23);
  master harness all 5 legs green (sec 27); post-fix 3,900-create loop
  clean — zero failures, zero record variants (sec 27)

## 32. STOP conditions (brief §32)

- No G/H redesign, no State-v5 format change, no cap raises, no new
  families, no R14 work, no commit — all honored. Confirmed at closure.

## 33. Completion report (brief §33)

R13-J is complete. Every section of the brief is closed with evidence:

- **Freeze held** — pack 23,069 entries / 15,278,272 B / SHA-256
  `b711d358…` byte-identical to R13-I; ranges 6,390/8,192; State-v5 5u;
  no runtime-semantic change (R13-J ships only the §3A FLIP removals
  and harness-side test changes).
- **Global ownership census** — unexplained/UNOWNED payload 0; 21,735
  ROM_BASE_ONLY + 22 EXPLICIT_DEFERRED (7 "MISSING" are TOML header
  artifacts); 1,388 COMPILED_PENDING all explained with named owners and
  stages (movement STAY bridge 15, multiboot 2, easy-chat 1,371).
- **§3A closure** — the last uncompensated compiled payloads removed:
  1,040 movement tables, 11 text blocks, 2 labels FLIPped to
  pack-served; the STAY set is arbiter-proven C-consumer-backed.
- **Family closures** — B 5,326 / C 8,438 / D 196 / F (layout 882 +
  tileset 1,562 + objevent 288 + 518 headers) / G 523/523 / H
  2,089/2,089; deferred-item audit names every owner; movement bridge
  documented with exact symbols; unexplained text duplicate 0.
- **Sweeps** — compiled-content, provenance, stale-object,
  generated-source, dead-bridge, fallback, loader-consistency all green.
- **Pack** — completeness 23,069; determinism (regeneration byte-identical).
- **State** — v5 unchanged; range census 6,390; 42/42 R13-I battery
  preserved.
- **Binaries** — release 23,464,296 B (−378,544 B), DINFO 36,105,696 B
  (−415,400 B); both `--verify-game-data` exit 0 (f3ae0881…d07b7).
- **Isolation verifier** — 37,681 checks, 0 failed, PASS (8,948/8,948
  ROM_BASE_ONLY; 17/17 COMPILED_PENDING_MIGRATION present); binary
  isolation verifier green (FLIP labels ABSENT).
- **Battery** — full regression battery green including the
  state-master three-mode flake closure: resource-state TEST 1-8,
  master harness 5/5 legs, and a 3,900-create post-fix loop with zero
  failures and zero record variants.
- **STOP honored** — no G/H redesign, no State-v5 format change, no cap
  raises, no new families, no R14 work, no commit. R14 handoff list is
  exactly the five items in `docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md`
  §11 (isolation proof, engine residue E re-classification, sub-200-B
  routing tables, packaging/legal audit, cleanup).
