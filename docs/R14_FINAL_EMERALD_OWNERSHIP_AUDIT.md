# R14 — Final Whole-Project Emerald Ownership Audit

**Status: AUDIT COMPLETE — STOP with evidence (per brief §34); see §33–§35.**
**Date:** 2026-08-25
**Predecessor:** R13-J (`docs/R13J_FINAL_OWNERSHIP_ISOLATION_REPORT.md`, tag `checkpoint-r13-complete`)
**Qualified ROM:** `../pokeemerald-reference/pokeemerald.gba` — SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`

## 1. Freeze R13-complete baseline (brief §1)

| Pin | R13-J value | R14 verified |
|---|---|---|
| Pack | 23,069 entries / 15,278,272 B / SHA-256 `b711d35877332c43ce671caacda7640905205579a367e8d60bc683c5d0aac5bb` | byte-identical (sha256sum) |
| Ownership census | unexplained 0; 21,735 ROM_BASE_ONLY / 1,388 COMPILED_PENDING / 22 EXPLICIT_DEFERRED | re-derived below (§21) |
| State-v5 | version 5u | 5u (`src/platform/native_state.c:50`) |
| Live ranges | 6,390 / 8,192 | ranges test green |
| Release binary | 23,464,296 B | on disk (23,464,296 B) |
| DINFO binary | 36,105,696 B | rebuilt: **36,105,696 B (zero delta)**, verify exit 0 |
| ROM verify | f3ae0881… exit 0 | **release + DINFO both exit 0** |
| R13-J isolation | 37,681 checks / 0 failed | battery re-run: **freshness-guard refusal** (binary mtime < newest source mtime after the §6 generator edit) — re-run against the §27 rebuilt binary |
| Regression | battery green | **37/38 green**; the 1 non-green is the isolation freshness guard above (not a content failure) |

## 2. Five R13 §11 handoff items (verbatim) and dispositions (brief §2)

Verbatim from `docs/R13_CONTENT_OWNERSHIP_ARCHITECTURE.md` §11 — R14 contains **only**:

1. **final global isolation proof and link-ownership report across both binaries** — disposition **A** (closed in R14): the whole-binary data-symbol census (§3–§4), forward/reverse hunters (§5–§6), transformed/generated audits (§7–§8), and the R14-mode isolation verifier (§22) constitute the global proof; the link-ownership report is the source-file → symbol census below (§4). The forced builds produce the fresh link maps for both binaries.
2. **the remaining engine residue classified E** (command tables, `gSpecials`/`gStdScripts`/`gSpecialVars`, compat seam descriptors, `host_data` scratch) — these are **not** migrated, only re-classified — disposition **B** (engine-owned, proof only): re-classified in §13 with exact symbols/sizes/owners.
3. **sub-200-B shape-D routing tables explicitly allowed to remain compiled** — disposition **B**: exact list in §13 (gSpeciesIdToCryId 270 B is the largest; routing rows are 2–8 B each; the `gMapLayouts`/`gMapGroups` GBA-address routing tables are host structs).
4. **packaging/legal-content audit** — disposition **A**: §28 content-isolation statement.
5. **cleanup** (packaging, docs, source-tree overlay strategy per migration architecture §15) — disposition **A**: §32 documentation; the source-tree overlay strategy (pinned pret dependency + adapter overlay) is confirmed as the long-term plan; no source-tree restructuring performed in R14 (out of scope for an audit; the strategy is documented).

**No item requires class E** — R14 does not become R15. However, the whole-project audits (§3–§8) **discovered** substantial un-owned content (below), which is handled per brief §34 (STOP with evidence + separate-stage recommendation), not by migration inside R14.

## 3. Whole-tree ROM-content inventory (brief §3)

Method: whole-binary `nm` census of all 33,973 symbols; data symbols ≥ 16 B (14,289) source-mapped via the DINFO binary's DWARF (addr2line); every content-family pattern bucket resolved against the 12 ownership TOMLs (15,840 declarations) and the 7,953-label bundle index; assembly-sourced symbols (1,544) resolved to defining `.s`/`.inc` files; byte-level verification of every seam claim (zero-fill) and every payload claim (nonzero canonical bytes).

| Bucket | Count | Bytes | Notes |
|---|---:|---:|---|
| RESOURCE_PACK_OWNED | 23,069 pack entries | 15,278,272 (pack) | all families; compiled copies are zero-fill seams |
| ENGINE_OWNED | see §13 | see §13 | command tables, callbacks, seam descriptors, scratch |
| RUNTIME_DYNAMIC | — | — | bss scratch (framebuffers, buffers, gHeap) |
| NATIVE_DIVERGENCE | 1 table + §12 items | 12,360 + §12 | gEvolutionTable (12 divergent rows) + divergence census |
| PROVENANCE_METADATA | generated k*/s* tables | ~1.9 MB | keys/digests/offsets/GBA words (no payload) |
| TOOLING_ONLY | 0 | 0 | tools/ not linked |
| DEBUG_ONLY | 0 (release) | 0 | release has zero debug sections |
| **UNRESOLVED_ROM_CONTENT** | **0 after classification** | — | everything classified; the classification *surfaces* the un-migrated residue below |

**The central R14 finding — un-owned canonical content compiled in the native binary:**

| Group | Exact bytes | Pack equivalent? | R14 disposition |
|---|---:|---|---|
| gMonIcon_* (411) | 430,080 B | none | R15 gfx-leaf closure |
| gMonStillFrontPic_* (411) | 363,816 B | art-shared with pack front sheets (R9 §6) | R15 gfx-leaf closure / declaration |
| gMonFootprint_* (394) | 12,384 B | none | R15 gfx-leaf closure |
| gItemIcon_* + palettes (471) | 57,896 B | none | R15 gfx-leaf closure |
| gBattleAnimSpriteGfx_*/Pal_* (~640) | 118,988 B + | none (battle family owns scripts only) | R15 gfx-leaf closure |
| wallpapers + icons (17 + 6) | ~35,700 B | none | R15 gfx-leaf closure |
| UI/misc graphics.c payloads (title, naming, trade, pokenav, berry, storage, clock, chat, pokeblock, card, frontier pass, transitions, textbox, markings, message box) | ~322,000 B | none | R15 gfx-leaf closure |
| credits text (sCreditsText_*) | 2,256 B | none | R15 text closure |
| union-room link text (sText_*/sJPText_*) | 13,297 B | none | R15 text closure |
| decoration descriptions (DecorDesc_*) | 5,310 B | none | R15 text closure |
| gEvolutionTable | 12,360 B | none — **excluded by design** (diverged) | §12 divergence census (native-owned) |
| sDroughtWeatherColors | 49,152 B | none | §13 declaration (engine LUT) |
| gTrainerMoneyTable / gTrainerFrontPicCoords / gTrainerBackPicCoords | 314 B | none | §13 declaration (engine tables) |
| 27 C-owned text labels (mystery_event_msg.c 13 + text_input_strings.c 14) | ~857 B | none | **CLOSED in R14** — `stay-compiled` consumer declaration added (gen_text_family.py + consumers.generated.toml, +7 lines, pack-neutral, binary-neutral); R15 text-closure candidate |
| easy-chat words (1,008 resources) | compiled in 45 group tables | declared PENDING (engine carve-out) | §9 disposition |
| multiboot (2 programs) | 176,352 B | declared PENDING (documented deferral) | §11 disposition |
| movement STAY (7 tables + 8 objects) | 56 B + 8 objects | declared STAY (§3A) | §10 disposition |

Total un-migrated canonical surface ≈ **1.36 MB gfx/text** + easy-chat + multiboot, all now exactly classified. This is the §34 STOP evidence.

## 4. Whole-binary symbol census (brief §4)

- Release binary: 33,973 symbols; data symbols ≥ 16 B: 14,289 (assembly-sourced 1,544; C-sourced 12,745 via DWARF mapping).
- Sections: .text 0x403980, .rodata 0x7fb000, .data 0x106b4a0 (host_data span ≈ 8.2 MB zero-fill region), .bss 0x18ef480.
- **TOML-declared symbols compiled: 1,205** — byte-verified: 749 all-zero seam fill targets (fonts 8×32,768 B, tilesets, objevent gfx, gTrainers 41,040 B, gItems 27,144 B, gBattleMoves, gSpeciesInfo, frontier tables…), 441 `*_Layout` struct scalars (40 B each, 10 nonzero scalar bytes — the R13-F documented layout-seam design), 3 PENDING (multiboot ×2 + 1), 12 EXPLICIT_DEFERRED, **0 true stale duplicates**.
- **Text closure:** 1,282 compiled text symbols — 1,254 in COMPILED_PENDING bundles (STAY twins with verified C consumers: apprentice.h, tv.c, battle_dome.c, birch_pc.c, mauville_old_man.c, contest.c, cable_club.c, roulette.c, start_menu.c, battle_main.c…), 27 not-catalogued (finding above), 1 declared. **BUNDLE_ROM_BASE_ONLY compiled symbols: 0.**
- **Pending composition correction:** the R13-J "easy-chat 1,371" label was imprecise — the 1,371 pending text records = 1,008 easy-chat words + 36 data bundles + 328 map bundles (reconciled; see §9).
- **R13-J "70 labels" correction:** the §3A STAY set in the binary is 1,503 compiled text labels (the "70" was the arbiter-proven FLIP-decision subset); documentation reconciliation only.
- Every surviving compiled symbol has an owner + allowed-reason (§3 table + §13 census). **No broad "engine" bucket without evidence.**

## 5. Final canonical-byte hunter (brief §5)

Method: all 18,098 extracted payload bins; 9,328 ≥ 24 B hunted by exact `find` over the 23,464,296-byte release binary; hits resolved to owning symbol + section (file-offset→vaddr corrected).

| Class | Hits |
|---|---:|
| A — canonical payload leak | **0** |
| B — generated semantic index (provenance words) | kScriptRoutingBytes (5,749 B routing class, sole copy, documented) + kScriptBridges (12 B movement opcodes, documented) + gSongTable GBA-address rows + sScriptTargetWords — all named in §8/§13 |
| C — machine-code/degenerate coincidence | 29 (17 .text x86 runs + 12 `trainer_none` 40-B near-zero sentinel matching zero runs inside icon pixel data — R7B precedent) |
| D — explained compiled representation | 6 (4 pending-bundle STAY text labels + 2 multiboot programs) |

**A = 0.** Every hit owned.

## 6. Reverse hunter: compiled-to-pack (brief §6)

Every large compiled data object was checked against the pack: all pack-declared payloads are zero-fill seams (byte-verified); the compiled objects that look game-content-like and have NO pack equivalent are exactly the §3 un-owned residue table (gfx leaves + evolution + weather + small tables + 27 text labels). No content is compiled *in addition to* a pack-owned canonical — nothing was missed by the forward hunter.

## 7. Transformed-data audit (brief §7)

Classified transformed sets: (A) generated semantic indexes (kTextBundleIndex, kGameplayNativeResources, kPokemonBattleCompatSlots, sModules, script/battle reloc/boundary/export tables — keys+offsets+digests only); (B) required engine representations (host MapLayout/MapGroup structs with in-band scalars, gBattlerPicTable heap-offset frames, gItemEffect_* native param arrays — native *format*, canonical *values*, no pack resource); (C) duplicated canonical content that should be regenerated — **none found beyond §3 residue**; (D) intentional divergences (§12). No transformed set is declared safe merely because its bytes differ from the ROM; each has an owner above.

## 8. Generated-native table audit (brief §8)

Every generated table (script/text/battle/pokemon/object_event/tileset/layout/gameplay/leaf generators) stores metadata (keys, digests, offsets, provenance words, slot bindings) or zero-fill seam leaves. Exactly two payload embeddings: `kScriptRoutingBytes` (5,749 B — sole copy, pack has no routing resources, ROM-SHA-1 gated, consumed at seam staging) and `kScriptBridges` (12 B movement opcodes — documented plan §7.4 exception). **No hidden duplicate payload. No violations.** Follow-up noted for R15: emit routing bytes as pack resources (G2 routing sub-stage machinery exists).

## 9. Easy-chat final disposition (brief §9)

The 1,008 easy-chat word resources are genuine canonical ROM text (modifiable game content), compiled inside 45 `gEasyChatGroup_*` tables (`src/data/easy_chat/*.h`) reached through the C pointer table `gEasyChatWordsByLetterPointers` (easy_chat_words_by_letter.h:2570) — the engine carve-out documented at R13-J. Closing it requires the R13-D/E table-repoint pattern (seam republish + ownership flip), i.e. a real live-cutover step. **Disposition: genuine game content, must be pack-owned for mod/profile scoping; closing it is a cutover, not an audit fix — recommended to the R15 text/gfx closure stage** (§9's STOP-with-recommendation branch). R14 does not hand-wave it: the exact mechanism, sizes, and migration path are pinned above.

## 10. Movement STAY final disposition (brief §10)

Exact: 7 STAY tables, 56 B — `Apprentice_Movement_Leave` @0x1881c12, `BattleFrontier_BattleDomeLobby_Movement_PlayerEnterDoor` @0x1881c22, `BattleFrontier_OutsideEast_Movement_SudowoodoShake` @0x1881c26 (+4 more, data/movement_tables_native.inc, 33 lines) and 8 `sMovement_*` objects (6× rotating_tile_puzzle.c, 2× union_room_player_avatar.c). Runtime reason: recomp-local (native minigame/UI) movement definitions — not canonical ROM movement tables; the 1,040 canonical tables are pack-served (R13-J §3A FLIP). **No duplicate of pack-owned movement; owner = native divergence (recomp-local constructs), final classification: NATIVE_DIVERGENCE/engine-owned.**

## 11. Multiboot final disposition (brief §11)

- `emerald:multiboot/program/ereader` (12,512 B): canonical ROM provenance (mb_ereader.gba), **runtime-live** — `src/ereader_screen.c:405` (`EReader_Load`) loads the compiled program. Must stay compiled until the multiboot runtime seam exists (documented deferral, TOML binding). Owner: engine/runtime requirement.
- `emerald:multiboot/program/pokemon-colosseum` (163,840 B — a full Nintendo-published GBA ROM image, data/mb_colosseum.gba): canonical provenance, **natively dead** (`src/intro.c:1131` is inside `#ifndef PORTABLE`), compiled as a GBA-target payload retained in the native link. Owner: GBA-branch retention + documented deferral ("flip waits on the multiboot runtime seam").
- Both are outside normal Tallgrass mod/profile scope (hardware multiboot programs). **Closed: ownership explicit, both deferred by design; the Colosseum ROM image is flagged in §28 (packaging/legal-content) with the flip schedule recommended for R15.**

## 12. Native gameplay divergence census (brief §12)

1. **Evolution-rule fork** — `gEvolutionTable` (12,360 B, include/pokemon.h:355): differs from the qualified ROM in exactly **12 of 2,060 rows**: 4 `TRADE→LEVEL 40` (species 64 Kadabra→65, 67 Machoke→68, 75 Graveler→76, 93 Haunter→94) and 8 `TRADE_ITEM→ITEM` with identical (item, target) (species 61→186, 79→199, 95→208, 117→230, 123→212, 137→233, 373→374/375). All other 2,048 rows byte-identical to the ROM. The pack excludes evolution by design ("diverged evolution family", build_d1_pack.sh). Owner: intentional single-player adaptation (code/behavior, not a forgotten ROM asset).
2. **Trade-evolution item divergences** — subsumed by (1) (the 8 TRADE_ITEM→ITEM rows).
3. Native item-effect parameter arrays (`gItemEffect_*`, item_effects.h) — native *format* of canonical values; engine-owned.
4. The isolation verifier treats none of these as payload exemptions; they are code/table divergences with named owners.

## 13. Engine-owned data census (brief §13)

Exact compiled engine tables (all A-class, with owners):

| Table | Size | Owner/reason | Profile scope |
|---|---:|---|---|
| gScriptCmdTable (227×8) | 1,816 B | G script interpreter dispatch (handoff item 2, class E) | shared engine, per-game specials |
| gSpecials (526×8) | 4,208 B | special-call dispatch (E) | **Emerald-specific** (R15: profile seam) |
| gSpecialVars (22 ptrs) | 176 B | special var pointers (E) | Emerald-specific |
| gStdScripts (11 slots) | 88 B | zeroed; seam-published (E) | Emerald-specific |
| gMysteryEventScriptCmdTable | 136 B | MEVENT dispatch (E) | shared |
| gSongTable (610 rows) | 4,880 B | ROM logical addresses only; arena-served payload | Emerald-specific |
| gMapLayouts (441×4) + gMapGroups (22) | 1,764 + 2,124 B | GBA-address routing index (shape-D, sub-200-B rows — handoff item 3) | Emerald-specific |
| kBrailleTextAddresses (22) | 88 B | braille provenance pairs | Emerald-specific |
| gSpeciesIdToCryId (135 u16) | 270 B | cry routing (shape-D) | Emerald-specific |
| gBattlerPicTable_* / gBattleBgTemplates / gBattleWindowTemplates / gBattlerSpriteTemplates / gBattleScriptingCommandsTable / gBattlerControllerFuncs / gBattlePalaceNature* | ~2.6 KB | battle engine tables | shared engine |
| gTrainerMoneyTable | 112 B | class→money factors (canonical, undeclared — **R15 declaration**) | Emerald-specific |
| gTrainerFrontPicCoords / gTrainerBackPicCoords | 186 + 16 B | pic coordinates (canonical, undeclared — R15 declaration) | Emerald-specific |
| sDroughtWeatherColors | 49,152 B | canonical weather LUT, INCBIN, no pack resource (documented R13-A §5.5) — **R15 declaration** | Emerald-specific |
| gMonFrontPicTable/BackPicTable/StillFrontPicTable/PaletteTable/ShinyPaletteTable/IconTable/FootprintTable + coords | ~35 KB | gfx index tables (NULL-sentinel rows published by seam) | Emerald-specific |
| gMonFrontAnimsPtrTable + sAnim_* + sAnims_* | ~30 KB | anim frame metadata | Emerald-specific |
| gItemIconTable (378×16) | 6,048 B | icon index over (un-owned) leaves | Emerald-specific |
| gItemEffectTable + gItemEffect_* (70) | ~800 B | native item-effect params | shared engine, Emerald values |
| gObjectEventGraphicsInfo_* (245×56) | 13,720 B | objevent metadata structs | Emerald-specific |
| gFieldEffectObjectTemplate_* + sPicTable/sAnimTable | ~3.5 KB | FE sprite metadata | Emerald-specific |
| sWallpapers (17×24) + sWallpaperColors | 476 B | wallpaper index over (un-owned) leaves | Emerald-specific |
| sCreditsEntry_* + pointer table | 4,480 B | credits scroll metadata | Emerald-specific |
| gMovementActionFuncs_* (~200) | ~3.2 KB | movement-engine dispatch | shared engine |
| seam descriptors: kScriptRelocs/Boundaries/DynamicTargets/Exports/Pool/Modules/FBindings, sModules, sRelocs, sBoundaries, kTextBundleIndex/NativeResources/SlotBindings, kGameplayNativeResources, kPokemonBattleCompatResources/Slots, kLeafNativeResources, kMapHeaderKeys/Events/Connections, kEncounterHeaderKeys, kFrontierTrainerKeys, kPokedexRowKeys… | ~1.9 MB | generated semantic indexes (keys/digests/offsets) | family machinery shared; data Emerald |
| host_data scratch: __start_host_data span | ~8.2 MB | zero-fill seam region (fonts, trainers, tilesets, gfx targets) | shared mechanism |
| runtime scratch (sComp*/sExpanded*/framebuffers/gHeap/FLASH/VRAM/sRangeIndex/sHostPointers/sHostFunctions/sPersistentFunctions/sCaptureResourceRecords) | ~3.5 MB | native runtime buffers | shared |

No table carries canonical payload bytes without either a pack key (zero-fill seam) or a named owner above. The three undeclared canonical tables (money/coords/weather) get explicit R15 declaration entries.

## 14. Profile-specific vs shared-engine audit (brief §14)

- **Shared Tallgrass engine:** resource codec/reader/resolver/writer (src/gen3/resources/*), LZ77, TOML, pack builder, state walker core (native_state.c), image-pointer model, task/sprite sidecars, script/battle VM interpreters (command dispatch — per-game command TABLES are profile data), render/audio runtime.
- **Emerald profile-specific (pack-owned, seam-published):** all R13 families (text/scripts/maps/gameplay/gfx/audio) — already profile-scoped via keys.
- **Emerald profile-specific compiled (audited in §3/§13):** the §3 residue table + §13 Emerald-specific engine tables. **Every retained profile-specific canonical payload is either a zero-fill seam, a documented STAY/pending/deferral, an intentional divergence (§12), or the §3 un-owned residue → R15.** Nothing else remains compiled.
- FireRed integration: no refactor performed; abstraction seam list in §29.

## 15. Build/link isolation from ROM (brief §15)

- Linker inputs: no `.o`/`.bin` extracted payloads enter the link (Makefile_pc gates: DATA_ASM_OBJS filter-outs for map_events.o/mystery_gift.o, audio sweep, G/H region gates; DESKTOP_EXTERNAL_GAME_CONTENT flips INCBIN sources to HOST_DATA declarations).
- No objcopy/incbin of extracted payloads in the native link (INCBIN uses remain only for the §3 residue art + the GBA-only branches).
- The pack is loaded from disk at runtime (`Gen3ResourcePack_OpenFile`); no ROM byte array is linked into the executable.
- `--verify-game-data` is a runtime check against the qualified ROM, not a build input.
- The 37,681-check isolation runner + preprocessed-source sweeps + make dry-run sweeps (all green at R13-J; re-run in the battery) prove the executable does not embed extracted payloads beyond the §3 residue.

## 16. Runtime pack-required proof (brief §16)

Covered by the battery suites (GREEN): runtime-loader boot-refusal without the pack (no compiled fallback — G6/H7 structural), H7 bytecode-module-removal fault, corrupt-matrix 13 kinds (bad-tag/oob/bad-key/bad-role/bad-schema/bad-type/oob-resource-offset/oversized-count/duplicate-fields/bad-reserved/truncated-sidecar/bad-sidecar-size/raw-crc), missing-session/v4-unsupported/fingerprint-mismatch policies. Minimum allowed boot behavior before refusal: engine boots to the point of session registration; any family miss ⇒ terminal refusal with full rollback (documented at R13-I §17).

## 17. Wrong-ROM / wrong-pack isolation (brief §17)

- Wrong Emerald revision: profile registry resolves by name "bpee01-rev0"; an unknown profile name refuses (loader policy).
- Corrupt pack digest / missing pack / wrong profile identity: the pack header + manifest identity checks refuse; the runtime loader validates the pack hash + provider list.
- Schema/type/role mismatch: h3-faults #16 + corrupt kinds refuse pre-mutation (R13-I §17).
- Address-redirect fallback does not exist (key-identity resolution only).
Suite results: GREEN (R14 battery — profile/identity refusal paths, corrupt-pack/CRC/malformed-container refusals; no address-redirect fallback in any suite).

## 18. Profile identity audit (brief §18)

- Importer: per-family manifests pin `rom_profile = "bpee01-rev0"`; extraction keyed by profile.
- Pack manifest: profile name + game id in header.
- Runtime loader: provider id `"emerald.rom-base.bpee01"` (precedence 300); session fingerprint carries `gameId`.
- Save/state: buildId + content fingerprint (gameId-derived); v5 format profile-agnostic.
- Resource keys: `emerald:` namespace generated in the family generators (not in shared code).
- **Engine-global "Emerald assumptions" found:** none in `src/gen3/` (verified: zero emerald/bpee01 references); the assumptions are confined to `src/emerald/resources/` + family generators.
- **Exact FireRed-abstraction seam list:** (1) emerald_rom_profile registry + provider id; (2) `emerald:` key namespace in each family generator; (3) the 20+ `emerald_*_compat.*` seam modules; (4) gameId constant in session fingerprint; (5) GBA provenance constants (script GBA base 0x081dc2cc-class, EWRAM bases) per profile; (6) `EMERALD_SCRIPT_STATE_*` adapter enums consumed by the generic walker; (7) species/move/item ID spaces + schema tables (shared schema IDs, per-profile values); (8) gSpecials/gSpecialVars/gStdScripts + command tables (shared VM, per-game dispatch tables).

## 19. Pack completeness final audit (brief §19)

- 23,069 entries, 15,278,272 B, `b711d358…` — verified on disk.
- Deterministic rebuild (×2) reproduces the exact hash and `--check` passes ("byte-identical to the deterministic rebuild") — no orphan/duplicate-key/manifest drift possible (the pack builder validates manifest↔catalog consistency or fails).
- Every required ROM_BASE_ONLY key present: the 78,524-check runtime-loader census (battery green) verifies family completeness; the isolation runner re-runs against the §27 rebuilt binary (freshness-guard refusal in the battery, §22).
- Pack count change in R14: **none** (and none recommended inside R14; the R15 closure will add the §3 residue families).

## 20. Pack rebuild determinism (brief §20)

Two clean rebuilds + `--check`: entry count identical, byte size identical (15,278,272 B), SHA-256 identical (`b711d358…`). All generators regeneration is a no-op diff (TOML headers enforce it); `--check` paths green.

## 21. Ownership manifest final closure (brief §21)

| Category | Count |
|---|---:|
| ROM_BASE_ONLY (ownership_state) | 19,123 |
| COMPILED_PENDING_MIGRATION | 1,388 (= 1,008 easy-chat + 36 data bundles + 328 map bundles + 15 movement STAY + 2 multiboot — reconciled) |
| EXPLICIT_DEFERRED | 22 |
| bundle labels indexed (text) | 7,953 |
| compiled symbols declared (all TOMLs) | 15,840 |
| compiled data symbols in binary (≥16 B) | 14,289 |
| unexplained | **0** |

Every pending/STAY record carries its final rationale (§9–§11 + §3); every compiled symbol carries an owner (§3/§4/§13). The literal pending count intentionally remains nonzero (easy-chat, multiboot, 36+328 text bundles are outside pack ownership by documented design or deferred).

## 22. Final binary isolation verifier (brief §22)

R14 mode extends R13-J's 37,681-check runner with: the whole-project symbol census (§4), the canonical-byte hunter (§5), the transformed-data census (§7), the generated-table audit (§8), the ownership cross-check (§3/§21), stale-object sweep (§23), fallback-path sweep (§16), and the profile-specific compiled check (§14). Results: the R14-mode extensions (census/xcheck/byte-sweep/hunters, §3–§8) executed green on the current binary; the 37,681-check isolation runner was first refused by its freshness guard (binary predates the §6 generator edit — the generator does not enter the link), then failed on missing graphics intermediates (see §23), and **passed on the final rebuilt binary: 37,681 ok, 0 failed, PASS (8,948/8,948 ROM_BASE_ONLY isolated; 17/17 COMPILED_PENDING_MIGRATION present)**. `verify_binary_isolation.py` → ISOLATION GREEN (all sweeps incl. flip-text-* ABSENT).

## 23. Stale-object/build reproducibility (brief §23)

- `make -f Makefile_pc clean` (wipes build outputs + generated intermediates) → forced `-B rom` rebuild → **byte-identical**: SHA-256 `0601ad51007955c2a5b942724bd96e1f9f93b2ebffd7c7815dff76cfefe8059b` equals the pre-clean baseline; size 23,464,296 B; section layout unchanged. **No stale-object dependency.**
- Pack hash unchanged (`b711d358…`); generated artifacts (TOMLs, native tables) regenerated identically (all `--check` paths green).
- **Reproducibility dependency documented:** `make clean` removes all graphics intermediates (`*.lz/.4bpp/.gbapal…`); the forced native build regenerates only those its linked TUs consume, so the isolation runner's ~2,000 TOML-referenced raw artifacts (trainer front/back pics, object-event pics/palettes, pokemon gfx, battle-frontier dome anims) must be regenerated from the surviving `.png` sources via the same make rules (`xargs make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 -k -j8` over the TOML artifact list) before the runner can hash payloads. Regeneration is deterministic; after it the runner passes 37,681/0 (§22).

## 24. State-v5 final regression (brief §24)

**GREEN** (R14 battery): resource-state TESTS 1-8 (13-kind corrupt matrix + v4/unsupported/fingerprint/missing-session policies) passed; state master harness all 5 legs green (G fresh-process vaddress 8/8, H battle fresh-process, H live suite ASan/UBSan, mixed-family 24 records, deterministic serialization); resource-ranges green (6,390 pin); session-fingerprint green; script/battle state + cross-restart + faults suites green. State-v5 unchanged: version 5u, 6,390 ranges, no format/cap change (R14 changed no runtime code).

## 25. Full gameplay regression (brief §25)

**GREEN** (R14 battery): runtime-loader (78,524+ checks), trainer-compat + production + sanitize, layout, tileset, object-event, script-compat + sanitize, script-faults 21/21, script/battle module loaders (523/523, 2,089), real-tables, native-world real/neighborhood/render-proof, native-overworld renderer + sanitize, native-state regression, resource-lz + sanitize, resource-import + sanitize, rom-base-provider + sanitize, desktop real-SDL probe. Covers boot/overworld/maps/text/NPC/scripts/battle/anim/AI/audio/trainer/encounter/Frontier/Pokédex/save-state automated flows; no behavior change expected or observed.

## 26. Full sanitizer battery (brief §26)

**GREEN, zero findings** — ASan/UBSan suites in the battery: battle-live sanitize (H4/H5/H6 legs), trainer-sanitize, script-compat-sanitize, resource-lz-san, resource-import-san, rom-base-provider-san, native_overworld_sanitize. All clean.

## 27. Forced release and DINFO (brief §27)

Forced builds via `make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 -B rom` (never `make -B linux64`):

| Build | R13-J | R14 | Delta |
|---|---:|---:|---:|
| Release (`-B rom`) | 23,464,296 B | 23,464,296 B | **0 B** (byte-identical; SHA-256 `0601ad51…` reproduced across clean/forced rebuilds) |
| DINFO (`-B rom DINFO=1`) | 36,105,696 B | 36,105,696 B | **0 B** |

Section deltas: none (byte-identical builds). Removed stale content: none (R14 removed no linked content — the only source change is the §6 generator row, which does not enter the link). Release: zero DWARF debug sections (`.symtab` only). DINFO: 7 debug sections. Both `--verify-game-data` → `f3ae088181bf583e55daf962a92bb46f4f1d07b7` exit 0. **The canonical release binary is left on disk** (`pokeemerald-linux64`, 23,464,296 B, `0601ad51…`).

## 28. Final legal/content-isolation statement (brief §28)

Engineering statement (not legal advice):
- **Pack/ROM-derived canonical content:** all 23,069 pack entries (text, scripts, map metadata, gameplay tables, gfx leaves of the migrated families, audio, battle modules) are ROM-derived and live ONLY in the pack; their compiled footprints are zero-fill seams or metadata indexes.
- **Compiled native content that remains engine/runtime-owned:** the §13 census (dispatch tables, semantic indexes, native-format tables, scratch buffers) plus the documented intentional divergences (§12).
- **Compiled native content that is canonical ROM-derived and NOT pack-owned:** the §3 residue table (~1.36 MB gfx/text + easy-chat words + 2 multiboot programs incl. one full Nintendo-published GBA ROM image + 27 text labels + 3 small gameplay tables). None of it is hidden metadata: each byte is accounted in §3/§13. It is technically ROM-derived game content compiled in the executable — the exact gap R14 exists to close, now measured and named; closing it requires the R15 stage (see §34/§35).
- Outside the §3 residue and the documented provenance-word metadata, the executable contains no known canonical ROM-derived game-content payload.

## 29. FireRed readiness handoff (brief §29)

No FireRed work performed; no FireRed code modified. Readiness inventory:

**Shared engine infrastructure already reusable:**
- `src/gen3/resources/*` — pack codec/reader/writer/resolver/providers, LZ77, SHA-1/256, TOML, resource core/ids/types/catalogs (verified profile-clean: zero Emerald references).
- `src/platform/native_state.c` — the State-v5 walker, image-pointer model, sidecars, transaction/atomicity, corrupt matrix, fingerprint (generic; consumes per-game adapter enums).
- The G script VM + H battle VM interpreters and their reloc/boundary/export metadata machinery (command dispatch is table-driven; tables are per-game).
- Render/overworld/audio runtimes and the resource import pipeline (rom-base provider, manifest/catalog import, multi-manifest pack builder).

**Emerald-specific runtime seams needing profile abstraction (exact list, from §18):**
1. `emerald_rom_profile.c/h` — profile registry ("bpee01-rev0"), provider id `emerald.rom-base.bpee01` (precedence 300).
2. The `emerald:` key namespace emitted by every family generator (text/gameplay/script/battle/tileset/layout/object_event/pokemon/audio/leaf).
3. The `emerald_*_compat.*` seam modules (audio, battle, battle_live, battle_state, encounter, frontier, gameplay, layout, leaf, map, object_event, pokedex, pokemon_native, resource_compat/import/ranges/session, script compat/state).
4. The session fingerprint `gameId` constant.
5. GBA provenance constants per profile (script GBA base 0x081dc2cc-class, EWRAM bases, arena GBA addresses).
6. `EMERALD_SCRIPT_STATE_*` adapter enums consumed by the generic state walker.
7. Species/move/item ID spaces + schema-table values (schema IDs shared; values per profile).
8. `gSpecials`/`gSpecialVars`/`gStdScripts` + script/battle command tables (shared VMs, per-game dispatch tables).

**Shared resource schemas:** fonts (8/1), text (9/1, 16/4), arena types 16, audio (10/11/15), gfx (2/3/4), maps (16/41-44), G script (16/45-46), battle (16/47-51), trainer (16/17), encounter (16/20), frontier (16/22/37) — the schema *classes* are shared; per-profile values re-emitted by the family generators.

**Emerald-only schemas:** the concrete key/offset/count data under every schema (each family's generated tables are Emerald data).

**Pack/import pipeline reusable:** pack_build (multi-manifest, deterministic), rom-base provider, elf-manifest tooling, all family generators (parameterized per profile in principle; currently hardcode the Emerald ROM walk + key namespace).

**State-v5 generic pieces:** walker ladder, record classes, sidecar schemas, fingerprint, atomic commit, corrupt matrix, cross-process proofs, master harness.

**G/H VM infrastructure reusable:** the interpreters, arena staging, reloc/boundary metadata, fault matrices, state adapters (per-game bytecode + tables only).

**Known FireRed native-runtime blockers outside ownership scope:** none introduced by R14; the blockers are the seams above plus the un-closed R15 residue (which is Emerald-specific content, not shared machinery).

## 30. Tallgrass mod-platform readiness handoff (brief §30)

What the ownership system already provides (nothing implemented in R14):

- **Layered packs / overrides:** the provider registry (id + precedence) and ResolveByKey-only materialization already support provider stacking; the pack itself is a single production provider today.
- **Per-profile mod scoping:** resource keys are `emerald:`-namespaced with canonical ids per family; the session fingerprint (gameId + provider list + logical content digest) already refuses cross-content loads cleanly — the documented no-force-load policy is the foundation for mod identity.
- **Dependencies/load order:** provider precedence (300 today) + idempotent range registration (key-removal preamble, F1/F3) are the existing ordering primitives; load-order policy is unbuilt.
- **Resource fingerprints:** header buildId + content fingerprint + per-resource digests (32-B SHA-256 keys, source hashes in TOMLs) are in place; a full mod fingerprint/version system is explicitly deferred (R13-I §19).
- **Future ROM-hack import:** the importer (multi-manifest + catalogs, extraction profiles, deterministic pack builder) is the import pipeline; per-profile profile registry is the extension point.

The R15 residue closure (gfx leaves + text + easy-chat) is a prerequisite for "all Emerald canonical content is modifiable", but not for the engine-level features above.

## 31. Manual final Emerald validation checklist (brief §31)

Interactive on the DINFO build (release first), pack in place — the human-visible gate (R10/R13-J precedent); automated coverage = the battery (§24–§26) + `--verify-game-data`:

**BOOT/OVERWORLD** — 1. boot cleanly · 2. load existing save · 3. several maps/interiors · 4. NPC dialogue · 5. signs · 6. movement scripts · 7. warps/transitions
**GAME DATA** — 8. Pokédex · 9. party/summary · 10. items/mart · 11. trainer data · 12. encounters
**BATTLE** — 13. wild battle · 14. trainer battle · 15. status/stat moves · 16. animations · 17. AI turns · 18. switch/faint/end battle · 19. post-battle return
**STATE** — 20. State-v5 overworld · 21. mid-battle · 22. mid-animation · 23. fresh-process load
**OTHER** — 24. Frontier representative path · 25. audio/music/SFX · 26. representative field effects
Plus the **fail-closed probes**: delete/rename a pack module → NOT_PUBLISHED (no crash); withhold a routing row → BOUNDARY_INVALID; wrong-pack/corrupt pack → clean refusal.
Look for: missing assets, incorrect data, stale pointer crashes, fallback behavior, state-load failures, broken routing.

R14 changed no runtime surface (audit + one metadata declaration + forced rebuilds), so the R13-J checklist is the valid script; the §3A-flipped surfaces (movement + FLIP text) remain the new-surface focus.

## 32. Documentation (brief §32)

This report (`docs/R14_FINAL_EMERALD_OWNERSHIP_AUDIT.md`). Architecture/status doc update: the §33 gates that can pass for an audit-only stage are green; the "profile-specific canonical content compiled" gate fails by the §34 STOP (1.36 MB + easy-chat + multiboot residue). The status block is therefore updated to "R14 AUDIT COMPLETE — STOP with finding; R15 gfx-leaf + UI/text closure recommended" — NOT "R14 COMPLETE" (the brief's "only after all hard gates pass" wording).

## 33. R14 hard completion gates (brief §33)

- five §11 handoff items reconciled: ✓ (§2 — 1/4/5 closed as A, 2/3 proven as B)
- unresolved ROM-derived content: ✓ 0 (§3)
- unexplained ownership: ✓ 0 (§21)
- unexplained canonical payload in native binary: ✓ 0 (§5 A=0)
- transformed duplicate game content: ✓ 0 unexplained (§7)
- profile-specific canonical content still compiled: **✗ ~1.36 MB + easy-chat + multiboot — exactly classified, not migrated** (§3) → §34 STOP
- easy-chat disposition: ✓ proven (§9 — R15 cutover recommended)
- movement STAY disposition: ✓ proven (§10)
- multiboot disposition: ✓ proven (§11)
- pack deterministic + complete: ✓ (§19/§20)
- pack entry count reconciled and exact: ✓ 23,069
- runtime compiled fallback: ✓ 0 for completed families (§16)
- stale object influence: ✓ 0 (clean rebuild byte-identical, §23)
- State-v5 unchanged and green: ✓ (5u, battery green, §24)
- range count expected/reconciled: ✓ 6,390 (§24)
- final R14 isolation verifier 0 failures: ✓ (37,681 ok / 0 failed on the rebuilt binary, §22)
- full regression green: ✓ (§25)
- sanitizers green: ✓ (§26)
- forced release/DINFO green: ✓ (zero deltas, both verify f3ae0881…, §27)
- FireRed handoff: ✓ (§29)
- Tallgrass handoff: ✓ (§30)

## 34. STOP conditions (brief §34)

**TRIGGERED:** the whole-project audits discovered a substantial set of un-owned canonical ROM content families (~1.36 MB gfx/text across 10 groups + easy-chat + multiboot, §3 table). Per the brief this is the "substantial new ROM content family discovered" STOP class — migrating it is a new migration wave (extraction manifests, pack generation, seam cutovers, ownership flips), which R14 must not silently become. **R14 therefore: completes the audit (this report), applies no migration of these families, does not declare the migration fully complete, and recommends R15 "gfx-leaf + UI/text closure + easy-chat/multiboot cutover"** with the exact inventory as its scope.
- No G/H redesign, no State-v5 format change, no cap raises, no pack architecture change, no FireRed work, no commit — all honored.
- Narrow fix applied inside R14 (per "small final duplicate → fix narrowly"): the 27 C-owned text labels (~857 B) now carry a `stay-compiled` consumer declaration in `consumers.generated.toml` (generator row added; regeneration verified no-op for every other output; post-fix `--check` green). Pack-neutral, binary-neutral, census closed. The remaining narrow candidates (3 small gameplay tables 314 B + weather LUT declaration) are folded into the R15 scope alongside the §3 residue — declaring them requires gameplay-family generator changes, which belong with the R15 closure rather than half-applied here.
- Prior R13 regression: the audit found no R13 runtime regression; the only reconciliation items are documentation-level (pending-label imprecision, "70 labels" wording).

## 35. Completion report (brief §35)

- **Files changed:** `tools/gen3_resources/text_family/gen_text_family.py` (+1 consumer_sites row), `resources/extraction/emerald/bpee01/text/consumers.generated.toml` (+7 lines, regenerated; post-fix `--check` green), `docs/R14_FINAL_EMERALD_OWNERSHIP_AUDIT.md` (this report), architecture/status doc updates (§32). No runtime code, no pack, no binary changes.
- **Five §11 handoff dispositions:** 1 global isolation proof + link-ownership report = closed (A); 2 engine residue re-classification = proven (B, §13); 3 sub-200-B routing tables = proven (B, §13); 4 packaging/legal-content = closed (A, §28); 5 cleanup = closed (A, §32 — strategy documented, no restructure). No E.
- **Final ownership totals:** 19,123 ROM_BASE_ONLY + 1,388 COMPILED_PENDING (1,008 easy-chat + 36 data bundles + 328 map bundles + 15 movement STAY + 2 multiboot — reconciled) + 22 EXPLICIT_DEFERRED; 7,953 bundle labels; 15,840 declared symbols; 14,289 compiled data symbols (≥16 B) all owned; unexplained 0.
- **Remaining pending/STAY:** easy-chat (R15 cutover), multiboot ×2 (documented deferral; Colosseum flip schedule recommended), 36+328 pending text bundles (STAY twins with C consumers), movement STAY 15, the 3 small undeclared gameplay tables + weather LUT (R15 declarations).
- **Canonical-byte hunter:** A = 0; 6 D hits (explained), 29 C coincidences, 2 B provenance embeddings (named).
- **Transformed-data:** 0 unexplained duplicates; all generated tables metadata-only except the two documented sole-copy embeddings.
- **Engine-owned census:** §13 table (exact symbols/sizes/owners).
- **Profile-specific compiled-content result:** the §3 residue table — **~1.36 MB gfx/text + easy-chat + multiboot** compiled natively, all exactly classified, none migrated in R14 (STOP, §34).
- **Easy-chat:** genuine canonical content; R15 cutover recommended (§9).
- **Movement:** recomp-local constructs; final owner = NATIVE_DIVERGENCE (§10).
- **Multiboot:** both deferred by documented design; EReader runtime-live, Colosseum natively dead (§11).
- **Pack:** 23,069 / 15,278,272 B / `b711d358…` — deterministic (×2 rebuilds + `--check`).
- **State-v5:** version 5, ranges 6,390 — green (master harness 5/5, mixed-family, determinism, corrupt matrix 13/13, §24).
- **Final isolation verifier:** R14-mode = 37,681-check runner + census/xcheck/hunter scripts — **0 failures** on the rebuilt binary (§22).
- **Build reproducibility:** byte-identical across clean/forced rebuilds; SHA-256 `0601ad51…` (§23/§27).
- **Release/DINFO delta:** 0 B / 0 B vs R13-J (byte-identical builds; R14 changes no linked content) (§27).
- **Regression battery:** 37/38 green + isolation runner green after the documented intermediates regeneration (§22–§26).
- **Manual checklist:** §31.
- **FireRed readiness:** §29 (exact seam list; no FireRed work performed).
- **Tallgrass mod-platform readiness:** §30 (no platform implementation performed).
- **Is Emerald ownership migration fully complete?** **No.** The R14 audit proves everything *migrated* is cleanly isolated (0 leaks, 0 stale, 0 unexplained among declared families), and discovers that ~1.36 MB of canonical gfx/text plus easy-chat and multiboot remain compiled outside pack ownership. Per brief §34 (substantial new content families) R14 STOPS with this exact inventory as the R15 "gfx-leaf + UI/text closure + easy-chat/multiboot cutover" scope.
- **Blockers:** none technical inside R14's mandate; the R15 scope itself is the blocker for the "fully complete" claim.

STOP after R14. No commit. No FireRed integration begun. No Tallgrass platform implementation begun.
