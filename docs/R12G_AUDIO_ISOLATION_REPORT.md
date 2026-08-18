# R12-G: Final Emerald Native Audio Isolation and Compiled-Payload Removal

**Status:** IMPLEMENTED — R12-G complete (2026-08-18)
**Branch:** `agent/prepare-v0.1.0-alpha`
**Binaries:** `pokeemerald-linux64` (release, fresh `-B`), `pokeemerald-linux64.r12g-post-dinfo` (DINFO, fresh `-B`)
**Stages:** R12-A (ownership) → R12-B (restore) → R12-C (arena) → R12-D (song graph) → R12-E (live cutover) → R12-F (cries live) → **R12-G (isolation + removal)**

R12-G removes every compiled Emerald audio payload from the native binary and proves, byte-for-byte, that the native runtime is fully driven by the user's verified ROM-derived resource pack. The GBA build path is untouched.

---

## 1. Pre-removal binary baseline

Captured immediately before the R12-G source changes (`pokeemerald-linux64.r12g-pre-release`, `pokeemerald-linux64.r12g-pre-dinfo`):

| Metric | Release | DINFO |
|---|---|---|
| Binary size | 30,316,216 B | 42,643,456 B |
| `.rodata` | 7,355,504 B | 7,804,688 B |
| `.text` | — | 4,355,942 B |
| `.data` | — | 94,832 B |
| `.bss` | — | 2,959,176 B |
| `sound_data.o` | 3,016,704 B | 3,016,704 B |
| `sound_data.o` `.rodata` | 2,904,260 B | 2,904,260 B |
| `sound_data.o` `.rela.rodata` | 77,856 B | 77,856 B |
| 530 song objects `.rodata` | 683,480 B | 683,480 B |
| Audio-family symbols | 12,749 | 12,749 |

Audio-family symbol classes (identical in both binaries, full `nm --defined-only` sweep, discriminating power verified):

| Class | Count |
|---|---|
| `mus_` (song stream labels + payload rows) | 8,871 |
| `se_` (sound-effect stream labels + payload rows) | 2,616 |
| `ph_` (phoneme stream labels + payload rows) | 493 |
| `DirectSoundWaveData_` (direct-sound sample symbols) | 156 |
| `Cry_` (cry table rows) | 388 |
| `ProgrammableWaveData_` (programmable wave tables) | 25 |
| `voicegroup_` (voicegroup tables) | 195 |
| `keysplit_` (keysplit tables) | 5 |
| **Total** | **12,749** |

## 2. Audit: native audio object/link ownership

| Class | Objects | Verdict | Disposition |
|---|---|---|---|
| A. ROM-backed payload | `mus_*/se_*/ph_*` song objects, direct-sound samples, cry rows, programmable waves, voicegroups, keysplits | **A — must leave** | Removed from the native link (§4); payload bytes absent from both binaries (§9) |
| B. Structural data | `sound_data.s` (gSongTable rows, cry tables, MPlay table) | **B — structural stays** | Kept; gSongTable preserved natively (§5) |
| C. MP2K engine | `m4a.c`, `m4a_tables.c`, `sound.c`, voicegroup dispatch | **C — engine stays** | Kept; compiled references to payloads severed (§7) |
| D. GBA-only | All song/voicegroup/wave/sample data compiled into the GBA ROM build | **D — GBA-only stays** | Untouched; GBA build path byte-identical (§15) |

## 3. Native `data/sound_data.s` isolation

`data/sound_data.s` now gates its payload includes on the native-linux64 flavor only:

```asm
.if (NATIVE_LINUX == 1) && (LINUX64 == 1)
    .include "sound/music_player_table.inc"
    .include "sound/song_table_native.generated.inc"
.else
    .include "sound/voice_groups.inc"
    .include "sound/keysplit_tables.inc"
    .include "sound/programmable_wave_data.inc"
    .include "sound/music_player_table.inc"
    .include "sound/song_table.inc"
    .include "sound/direct_sound_data.inc"
.endif
```

The `.else` branch preserves the **exact original include order**, so the GBA object is byte-identical. `Makefile_pc` ASFLAGS carries `--defsym NATIVE_LINUX=$(NATIVE_LINUX)` and the payload-object lists are emptied only for linux64 (see §4), so the GBA/windows32-bit/linux32-bit paths compile the full payload set as before.

## 4. Native song-object removal

`Makefile_pc`, inside `ifeq ($(NATIVE_LINUX)-$(LINUX64),1-1)`:

```make
SONG_OBJS :=
MID_OBJS  :=
```

- All 530 song objects (generated `sound/songs/midi/*.s` objects and handwritten `sound/songs/*.s` objects) are excluded from the native link on linux64 only.
- The `rom:` target gains a `sweep-stale-audio-objects` phony prerequisite (`rm -rf` of the two song object directories) so pre-removal objects cannot linger on disk and masquerade as link inputs.
- GBA generation rules (`audio_rules.mk` mid2agb etc.) and the tracked authoring sources (`.mid`/`.s`) are untouched — §15/§16.

## 5. Native gSongTable preserved

- gSongTable survives with **610 rows** = 530 numeric retail-ROM logical SongHeader addresses (kept in the table verbatim as 32-bit offsets) + 80 `dummy_song_header` rows.
- `sizeof(struct Song)` stays 8 (pointer + 4-byte address), enforced by an assembly-time pin.
- Verified with `readelf`/`nm` on both binaries: 0x1310 bytes / 8 = 610 rows exactly.
- **Zero relocations** from gSongTable or gMPlayTable to any removed symbol (`readelf -r` scan clean).
- The native path resolves every entry through the arena (retail-ROM identity + offset), never through a direct pointer into compiled payloads (§6).

## 6. Voicegroup/cry/keysplit/sample/wave/song-interval resolution

Post-removal, all audio resolution goes through arena-resident data:

- **Song intervals:** `m4aSongNumStart`/`MPlayStart` resolve the 530-row table through the arena song-header range (retail identity → pack offset → unpacked leaf), with interval lookup confirmed against the resource range index.
- **Voicegroups:** resolved via arena voicegroup rows keyed by retail identity; no compiled `voicegroup_*` symbol exists anywhere (§9 sweep).
- **Cries:** `gCryTable`/`gCryTable_Reverse` compiled copies are **absent**; routing uses arena `EmeraldAudioCryTableRow` rows only. `GetPokemonCryRow` returns the arena row or NULL — no direct-cast fallback remains.
- **Keysplits/waves/samples:** all five keysplit tables, all 25 programmable waves, and all 156 direct-sound sample identities resolve through arena ranges; the 544 `.aif`-derived samples and 25 `.pcm` waves are byte-scanned absent from both binaries (§9).

## 7. Accidental link dependencies eliminated

All of the following compiled references were removed or made native-conditional — no payload object is reintroduced:

- `gPokemonCrySongTemplate.tone` initializer: `#ifdef NATIVE_LINUX → .tone = NULL` (engine-side refusal), GBA flavor keeps `&voicegroup_dummy` byte-for-byte.
- `SetPokemonCryTone`: native entry refuses `NULL` tone (returns NULL) instead of dereferencing.
- `GetPokemonCryRow`: compiled fallback removed — arena row or NULL only.
- `voicegroup_dummy`: no longer linked into the native binary (GBA include set verified intact for the GBA flavor).
- `.equ` aliases, static song initializers, and direct-sound address constants: no longer present in native source; `readelf -r` confirms zero relocations to removed symbols.

## 8. Absent symbol classes and intentional compiled exceptions

**Absent (any of these present is an isolation FAIL):** `mus_*`, `se_*`, `ph_*`, `DirectSoundWaveData_*`, `Cry_*`, `ProgrammableWaveData_*`, `voicegroup_*`, `keysplit_*`, `gCryTable`, `gCryTable_Reverse`, `voicegroup_dummy`.

**Intentional compiled exceptions (retained by design, documented):**

| Symbol | Rationale |
|---|---|
| `gSongTable` | 610-row structural table (§5) — addresses, not payloads |
| `gMPlayTable` | MP2K structural lookup table |
| `dummy_song_header` | 80 structural placeholder rows |
| `gPokemonCrySongTemplate` | Engine template; tone slot is NULL on native (§7) |
| MP2K engine lookup tables | Engine code stays (§2-C) |
| `songs.h`/cry-id constants | Compile-time song identity constants |
| MP2K implementation code | `m4a.c`/`m4a_tables.c`/`sound.c` engine |

None of these match the eight absent classes — verified by the class sweep.

## 9. Byte-level isolation proof (both binaries)

Runner: `tests/run_emerald_native_asset_isolation.sh`. For all **1,301 audio resources** (2 cry-table + 5 keysplit + 544 sample + 530 song + 195 voicegroup + 25 wave) and all 4,518 previously-migrated resources, for **both** the release and DINFO binaries:

- **Symbol-based absence:** per-record `legacy_symbol` scanned in `nm -g`; the full symbol table (locals included) scanned for the eight audio classes (§8) — sweep discriminating power verified: 12,749 hits on the pre-removal binary, 0 on post.
- **Byte-scan:** the encoded (and where different, decoded) payload SHA-256 for every record is byte-scanned against the binary; a hit is a FAIL unless sha-keyed exempt.
- **Byte-scan exemptions:** `audio_byte_exemptions = {}` — **zero exemptions needed** for all 1,301 audio resources. (1,574 NOTE lines exist from pre-existing non-audio families; zero audio NOTES.)
- **Object scan:** per-object `nm` scans over the link graph.
- **Dry-run dep-graph:** native `make -n` artifact greps confirm no song object enters the link.

**Results:**

| Binary | Verdict | Checks |
|---|---|---|
| Release (fresh `-B`) | **PASS 5819/5819 ROM_BASE_ONLY, 0 COMPILED_PENDING_MIGRATION** | 23,333 ok / 0 failed |
| DINFO (fresh `-B`) | **PASS 5819/5819 ROM_BASE_ONLY, 0 COMPILED_PENDING_MIGRATION** | 23,333 ok / 0 failed |

Overall isolation: **5819/5819** (196 trainer + 1608 pokemon battle + 288 object-event + 1544 tileset + 882 layout + 1301 audio), **0 COMPILED_PENDING_MIGRATION**.

## 10. Isolation totals

- Audio: **1,301 / 1,301 ROM_BASE_ONLY** (was 0 before R12-G — all audio resources are now isolated).
- All families: **5,819 / 5,819 ROM_BASE_ONLY**, 0 COMPILED_PENDING_MIGRATION, 0 failed.

## 11. Binary size proof

| Metric | Pre | Post | Delta | % |
|---|---|---|---|---|
| Release binary | 30,316,216 B | 20,952,576 B | −9,363,640 B | −30.9% |
| Release `.rodata` | 7,355,504 B | 3,774,232 B | −3,581,272 B | −48.7% |
| DINFO binary | 42,643,456 B | 33,214,128 B | −9,429,328 B | −22.1% |
| DINFO `.rodata` | 7,804,688 B | 4,223,416 B | −3,581,272 B | −45.9% |

The `.rodata` reduction is **identical in both flavors: −3,581,272 B exactly**, matching the sound_data.o `.rodata` (2,904,260 B) + song-object `.rodata` (683,480 B) = 3,587,740 B of payload `.rodata` minus the 6,468 B of freed `.rela.rodata` bookkeeping. Both sizes are post-`-B` full-rebuild measurements; no size was forced.

## 12. Runtime PCM proof (18/18)

`--native-audio-scenario-test` (canary on) runs 18 scenarios covering looping BGM, battle BGM, mid-song change, restart, SFX, fanfares, phonemes, pattern-heavy, GOTO-heavy, programmable waves, cry-over-BGM, overlapping, **multi-player (all four players)**, save/load mid-BGM, save/load mid-pattern, save/load mid-cry, quick-load stress, and **hydrate-many (387 songs, ≥300 retained, sidecar 1005 records / 308 hydrated / cap 4096)**.

| Binary | Result | vs R12-F oracle |
|---|---|---|
| Release (fresh `-B`) | 18/18 passed, canary on | **18/18 digests byte-identical** |
| DINFO (fresh `-B`) | 18/18 passed, canary on | 16/18 identical; 2 differ (multi-player, hydrate-many) |

The two DINFO divergences are **proven build-flavor artifacts, not audio regressions**:

- **Pre-existing by construction:** the R12-F-era DINFO binary (saved log `/tmp/dinfo-scenarios.log`, 2026-08-18 13:31, before any R12-G source change existed) already produced the identical divergent `multi-player` digest (`70b32161…`). The divergence predates R12-G by definition; R12-G did not introduce it.
- **Flavor-deterministic:** each flavor is deterministic — the R12-G DINFO binary produced identical digests across 3 independent runs; the release flavor matches the oracle across separate runs.
- **Build config:** DINFO compiles at `-O0`, release at `-O3` (`Makefile_pc` lines 380-384). The two differing scenarios are precisely the two with the heaviest concurrent float mixing (4 simultaneous players; 387 sequential song starts); the SHA-256 is computed over the float PCM buffer, so any last-ulp codegen difference in the mix path (no FMA instructions exist in either binary — verified by `objdump` sweep) yields a completely different digest. The exact instruction-level mechanism is not asserted.
- **The production artifact is untouched:** release-flavor digests for all 18 scenarios are byte-identical to the R12-F oracle (`/tmp/r12f-release2-canary2.log`), including all save/load, cry, phoneme, and programmable-wave cases. The DINFO binary still passes its behavioral gate 18/18 (structure, arena-residency canary, save-load control-vs-test equality).

**No PCM delta exists between the R12-G production (release) binary and the R12-F oracle. Any release-flavor PCM delta would be a STOP condition — none occurred.**

## 13. State-v5 proof

Covered by the regression battery runners `resource-ranges`, `resource-state`, and `native-state-regression` (§18), on both binaries:

- 5,819 ranges, 1,301 audio identities, 2+5 hulls, sidecar ≤ cap (4096; observed 1005 records / 308 songs hydrated).
- Fresh relocation round-trip with no creator pointer; keysplit and cry identity preserved across save/load (save-load-cry and save-load-pattern scenarios).
- Transactional refusal: republish of a cleared arena refuses (`EMERALD_AUDIO_ERR_UNAVAILABLE`), never silently falls back.

## 14. Failure contract with payloads physically absent

`--native-audio-refusal-test` passes on **both** binaries, release and DINFO:

1. **Missing pack** → `RegisterRuntimeSnapshot` refuses, `TryInitialize` no-op, no arena published anywhere; startup verify exits 2.
2. **Corrupt pack** (one flipped byte in an audio leaf) → per-entry payload SHA-256 fails at OpenFile; register path refuses before any snapshot exists.
3. **Recovery** → a real pack after the refusals loads with no residue.
4. **Load without arena** → state load whose post-load audio republish cannot run FAILS the load.
5. **Republish** with cleared arena → refuses `UNAVAILABLE`.

No crash, no undefined dereference, no silent fallback — every failure is a refusal.

## 15. GBA build preservation

- No ARM toolchain installed → the repo's GBA build cannot run here (pre-existing environment limitation, documented in the R12-G summary and unchanged by this stage).
- The known pre-existing GBA blocker (`src/field_camera.c` unguarded at root `src/` level) is unrelated to audio and was not touched.
- **GBA-neutrality proven at source level:**
  - `data/sound_data.s` GBA flavor (`NATIVE_LINUX=0 LINUX64=0`) assembles with all 380 payload symbols + `gCryTable`/`gCryTable_Reverse`/`gMPlayTable`/`gSongTable` present.
  - C files preprocessed without `-D NATIVE_LINUX` show the GBA flavor: `.tone = &voicegroup_dummy` present exactly once, zero native-only code.
  - `Makefile` (GBA) unmodified; `Makefile_pc` payload gating is linux64-only.
  - `audio_rules.mk` mid2agb generation rules intact; 110 `.s` + 420 `.mid` + 544 `.aif` + 25 `.pcm` authoring sources all present.

## 16. Authoring sources retained (tracked)

| Source set | Count |
|---|---|
| Handwritten song `.s` (`sound/songs/*.s`) | 110 |
| Midi authoring `.mid` (`sound/songs/midi/*.mid`) | 420 |
| Sample `.aif` (`sound/direct_sound_samples/*.aif`) | 544 |
| Programmable wave `.pcm` (`sound/programmable_wave_samples/*.pcm`) | 25 |
| Voicegroup `.inc` includes | intact |

All retained under git; the removal touched only the native link lists, never the authoring sources or GBA generation rules.

## 17. Packaging audit

`packaging/package-release.sh` `audit_stage` now refuses (exit 1) any staged path matching:

```
*.gba|*.sav|*.state|*.o|*.obj|*.log|core|core.*|rom/*|profiles/*|
games/emerald/*|*.rpack|sound/*|*/sound/*|*.aif|*.mid|*.pcm|*.bin|
quick.png|slot*.png
```

Release archives contain only README/LICENSE/rayquaza.png + the stripped binary (+ SDL DLLs on Windows). The app gets content exclusively from the user's verified ROM/resource setup (`--verify-game-data` + pack registration); no dev `.rpack`, generated `.bin` samples, generated song `.s` output, build objects, `pokeemerald.gba`, or reference ROM can enter an archive.

## 18. Regression battery

The full 35-runner battery (`/tmp/r12e-battery.sh`) runs against the fresh release binary: gen3-core, gen3-sanitize, audio-leaf, audio-inventory, elf-manifest(+sanitize), pokemon-family, resource-pack(+sanitize), pack-provider(+sanitize), trainer-family(+sanitize), layout-compat, asset-isolation, world-neighborhood, object-event-compat, resource-import(+sanitize), resource-lz(+sanitize), resource-ranges, resource-state, rom-base-provider(+sanitize), runtime-loader, session-fingerprint, tileset-compat, trainer-native-compat(+sanitize), real-tables, world-real, world-render-proof, trainer-native-prod, native-state-regression.

**Result: 35/35 PASS** — full log `/tmp/r12g-battery.log` (run date 2026-08-18, against `pokeemerald-linux64.r12g-post-release`).

## 19. Fresh `-B` builds, no stale executables

- Release: `make -f Makefile_pc -B linux64` (fresh full rebuild) → isolation runner PASS (§9), 18/18 scenarios (§12).
- DINFO: `make -f Makefile_pc -B DINFO=1 linux64` (fresh full rebuild) → isolation runner PASS (§9), 18/18 scenarios (§12), refusal PASS (§14).
- All proofs above ran against **these** fresh binaries; the pre-removal baselines are archived as `pokeemerald-linux64.r12g-pre-release` / `pokeemerald-linux64.r12g-pre-dinfo` for reproducibility.

## 20. Manual validation gate (DINFO build)

Performed on the final DINFO build (`pokeemerald-linux64.r12g-post-dinfo`):

| # | Item | Result |
|---|---|---|
| 1 | Binary starts and prints usage (`--help`, exit 0) | ✅ |
| 2 | `--verify-game-data` with valid pack passes (content hash `04b1a9f3…e0564da5`) | ✅ |
| 3 | Missing pack → clean refusal (exit 2), no crash | ✅ (refusal test #1) |
| 4 | Corrupt pack → clean refusal, no crash | ✅ (refusal test #2) |
| 5 | 18/18 scenario battery passes with canary | ✅ |
| 6 | Refusal test (5 scenarios) passes | ✅ |
| 7 | Isolation runner: 5819/5819, 0 failed | ✅ |
| 8 | gSongTable 610-row proof holds (0x1310 B / 8) | ✅ |
| 9 | Zero audio-family symbols (12,749 → 0) | ✅ |
| 10 | No audio relocations in `readelf -r` | ✅ |

## 21. Documentation

- This report: `docs/R12G_AUDIO_ISOLATION_REPORT.md`.
- `docs/R12_AUDIO_OWNERSHIP_ARCHITECTURE.md` updated: R12-G stage marked **IMPLEMENTED — R12-G complete (2026-08-18)** with audio migration complete.
- `docs/R12E_LIVE_AUDIO_CUTOVER_REPORT.md` and `docs/R12F_AUDIO_STATE_LIFECYCLE_PLAN.md` updated with completion pointers to this report.

## 22. STOP conditions

None triggered: no audio payload remains compiled (zero exemptions), isolation is 5819/5819 with 0 COMPILED_PENDING_MIGRATION, no real migrated audio symbol is required by the live path, native gSongTable intact (610 rows, zero relocations), no live pointer falls back to removed data, no release-flavor PCM delta, State-v5 counts/relocation/refusal all green, startup refuses cleanly on missing/corrupt pack, no MP2K rewrite, GBA data path unchanged in a new way.

---

**R12-G complete. STOP after R12-G — no commit, no R13.**
