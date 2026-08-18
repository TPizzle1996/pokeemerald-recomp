# R12-D Plan — Song Graph Implementation Plan

Stage: R12-D (530 MP2K song graphs) on the R12 audio migration path
(R12-A audit → R12-B leaves → R12-C structural → **R12-D songs** → R12-E
cries/SFX end-to-end → R12-F State v5 full → R12-G compiled-payload
removal). Review-only document: no implementation, no production changes,
no R12-E/F/G work. Baseline: `checkpoint-r12c-complete`.

Every count in this plan was derived from the qualified reference ELF
(`../pokeemerald-reference/pokeemerald.elf`) + retail-matching reference
ROM (SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7), cross-checked
against `sound/song_table.inc` and the MPlayDef.s command set, and proven
by a prototype stream walker run over all 530 graphs (all facts below
carry the result of that run).

---

## 1. Exact song inventory

- **530 unique song graphs** — confirmed: 530 exported header symbols
  (`mus_*`/`se_*`/`ph_*`, type R) in the qualified ELF; a bijection with
  the 530 non-dummy `gSongTable` rows (every song referenced exactly once,
  no song unreferenced).
- **Composition: mus 210 + se 269 + ph 51 = 530.** By music-player index
  (from `sound/song_table.inc`, 610 rows): BGM player 0 = 268 rows
  (188 `mus_*` + **80 dummy rows**), SE1 = 223 `se_*`, SE2 = 112
  (22 `mus_*` jingles + 39 `se_*` + 51 `ph_*`), SE3 = 7 `se_*` ambient
  loops. 188 + 22 = 210 mus; 223 + 39 + 7 = 269 se.
- **The 80 dummy rows are NOT resources**: they all hold the same 4-byte
  value `dummy_song_header` (0x086b5d00, four zero bytes, immediately
  after the table's last row; `song_table.inc:616-617`). Verified: 80
  rows → 0x086b5d00, 530 rows → 530 distinct song symbols.
- **Canonical source symbols**: the 530 header labels
  (`mus_battle_wild`, `se_use_item`, `ph_nurse_held`, …) plus 7,740 local
  track labels (`<song>_N`, `<song>_N_B1`, …) in the same objects.
  Sources: 110 hand-written `sound/songs/se_*.s` + 420 git-ignored
  generated `sound/songs/midi/*.s` (from 420 tracked `.mid` via
  `tools/mid2agb`, per-song options in `midi/midi.cfg`).
- **Key taxonomy**: `emerald:audio/song/<canonical>` (lowercase,
  `_`→`-`, the R11-B rule), type `music-sequence`, schema 1,
  representation `gba-mp2k-song-graph`.
- **Canonical bytes: 684,052 B total (0xA7014)** = mus 651,184 (0x9EFB0)
  + se 31,304 (0x7A48) + ph 1,564 (0x61C). (The R12-A audit's
  "~708,920 B" over-estimated; the exact contiguous partition is
  684,052 B, proven in §2.)
- **Sizes**: min 8 B (`mus_dummy` — the one zero-track song: header
  only), max 16,396 B (`mus_rg_credits`). Buckets: <256 B: 318,
  256–1K: 46, 1K–4K: 116, 4K–16K: 49, ≥16K: 1.
- **Track-count distribution** (header byte 0):
  {0: 1, 1: 195, 2: 122, 3: 10, 4: 8, 5: 8, 6: 15, 7: 37, 8: 51,
  9: 46, 10: 37} — consistent with `gMPlayTable` capacities (BGM ≤ 10
  tracks, SE1 ≤ 3, SE2 ≤ 9, SE3 ≤ 1).
- **Zero-length / alias / unusual cases**: none zero-length;
  `mus_dummy` (8 B, 0 tracks) is the only degenerate graph and it IS a
  resource (row 0 of the table, a real `mus_*` symbol). No graph aliases
  another (the dummy rows alias the *compiled* 4-byte header, not a song).

## 2. Song object boundaries (slice derivation — st_size == 0 problem)

All assembly symbols have `st_size == 0`, so sizes must be computed.
**Method (validated end-to-end, not a bare "next symbol" heuristic):**

1. **Header addresses** — the 530 exported header symbols from the
   qualified ELF (sorted by address). The header is the LAST 8+4·N bytes
   of its object (`trackCount` byte, `blockCount`, `pri`, `rev`, 4-byte
   `tone`, N×4-byte part pointers — byte-identical to `struct
   SongHeader`, m4a_internal.h:246-254).
2. **Per-song size** = 8 + 4·`trackCount`, where `trackCount` is ROM byte
   0 of the header (parses deterministically; range 0–10).
3. **Object start** = end of the previous song's object; the first
   object's start = its own first track label. **Packed-object invariant
   (proven)**: for all 530 songs, the object's first track label (7,740
   local labels cross-checked) equals the previous object's header end —
   **0 mismatches**. The objects form one contiguous block with zero
   inter-object padding:
   `slice[i] = [end[i-1], addr[i] + 8 + 4·trackCount[i])`.
4. **Independent anchors**: the block starts at 0x088FC03C — exactly
   2 bytes after the last R12-B leaf payload ends
   (`DirectSoundWaveData_sd90_enhanced_delay_shaku`, end 0x088FC03A;
   `.align 2`). The block ends at 0x089A3050, where non-audio data
   begins (Sio32 library: `llsf_struct`, `version_string`, `__clz_tab`,
   `gEReaderLinkData_Start`, …). Every byte of
   [0x088FC03C, 0x089A3050) belongs to exactly one song — the partition
   sums to 684,052 B exactly.
5. **Validation gates** (each one catches a different mistake class):
   - the **stream walker** (§6) requires every address operand to land
     inside the owning song's slice — a wrong boundary shows up as a
     dangling GOTO/PATT target;
   - the **three-way gate** (qualified ELF slice == retail ROM slice ==
     pack payload) re-proves every byte;
   - the **generated-source gate**: the 420 generated `.s` are
     reproducible from the tracked `.mid` + `midi.cfg` (audit §1.2); the
     assembled stream bytes are what the slice contains.

Note: the block straddles one ELF section boundary (section 6→7 at
0x088FD03C) — the packed-object rule, not section membership, is the
boundary authority; section layout serves only as the independent
sanity anchor above.

## 3. Canonical payload representation

- Payload = the verbatim ROM slice (§2), byte-for-byte GBA form. Confirmed:
  - `SongHeader` fields stay 4-byte GBA logical addresses (tone +
    N×parts) — they are never rewritten in the pack, the arena, or the
    ROM slice (architecture §4.1/§4.4).
  - GOTO/PATT operands stay 4-byte absolute GBA addresses in-stream
    (`MP2K_ADDRESS_OPERAND_SIZE = sizeof(GbaAddr) = 4`,
    include/music_player.h:9-19; `MP2KReadAddressOperand` reads exactly
    4 bytes).
  - No xwave in song graphs (0 occurrences — §6), but the walker still
    validates the encoding so a future stream that uses it is handled.
  - No host pointers enter pack data; no 64-bit widening in-stream —
    the pack payload, arena copy, and ROM slice are byte-identical by
    construction and by gate.

## 4. Arena placement

**Every song lies inside the current audio ROM span
[0x0867709C, 0x089A3DB4).** The song block [0x088FC03C, 0x089A3050) is a
strict sub-range. Therefore:

- `arenaOffset = romAddr - EMERALD_AUDIO_ROM_START` works with the
  EXISTING arena allocation: song offsets fall in
  [0x21BFA0, 0x32B614) ⊂ [0, spanSize=0x32CD18). **No arena resize, no
  rebase** — the R12-B/C zone already allocates the full 3,329,304-byte
  window; the song sub-range is currently unbacked holes (R12-B copied
  only the 569 leaves; the design always reserved the holes for songs —
  architecture §4.1 "holes left unbacked").
- **No collisions**: 569 leaf payload extents end at 0x088FC03A (2 bytes
  before the block); 5 keysplit copies sit below the leaf zone
  (0x086b4698+); the 197 structural labels sit below the zone base. The
  song block overlaps nothing — proven by extent intersection over all
  569 leaves (0 overlaps) and the packed-object partition.
- **Existing R12-B leaf offsets unchanged** (the leaves stay at their
  current arena offsets); **transformed-zone offsets unchanged** (the
  transform zone follows the verbatim zone; nothing moves). The arena
  header's size fields stay as pinned; only `payloadBytes`-style
  accounting gains the song bytes.

## 5. Logical-address resolution (the HostResolveGbaAddr extension)

**Decision: D — hybrid.** Keep the R12-C exact-start table; add ONE
interval mapping for the contiguous song block. This is exactly the
architecture's approved mechanism (§4.2 `HostMemoryRegisterLogicalRange
(GbaAddr romStart, GbaAddr romEnd, void *hostBase)` — the "one span for
the verbatim zone" it always specified), scoped to the song span in
R12-D (the leaf span's range is R12-E's).

- **New table**: `HostMemoryRegisterLogicalRange(0x088FC03C,
  0x089A3050, zoneBase + 0x21BFA0)` — sorted interval array, capacity 16,
  publish-time-only, cleared by the same lifecycle as the exact-start
  table. Lookup: binary search on `romStart` + one bounds check.
- **Why one span, not 530 per-song intervals**: the block is contiguous
  (0 inter-object gaps, §2); every address inside it is valid song
  content; resolution is identity-preserving (`base + (addr - start)`)
  regardless of which song owns the byte; per-song identity belongs to
  the R10 range index (state-v5, §9), not to the resolution table.
  Per-song intervals would add 530 entries and a second identity
  dimension to a table whose only job is address→host translation.
- **Why not one span for the whole verbatim zone** (0x0867709C–
  0x089A3DB4): the zone contains non-audio holes (gSongTable/
  gMPlayTable region, the Sio32 tail) whose bytes are NOT in the arena;
  a whole-zone mapping would resolve those addresses to unbacked arena
  bytes. The exact span [0x088FC03C, 0x089A3050) is precisely the backed
  song region. (R12-E will register the leaf sub-spans the same way.)
- **Precedence** (host_memory.c:364-405, extended):
  1. exact-start table (197 structural labels, binary search) — song
     header `tone` hits land here (R12-C behavior unchanged);
  2. **interval range(s)** (song block — NEW);
  3. handle table (0xE-prefixed u32s);
  4. direct-cast fallback (compiled song bytes stay at GBA-shaped
     sub-4 GiB addresses through R12-G — an unregistered address
     silently serves the compiled parity copy, the R12-B additive
     degrade).
- **No raw host addresses are stored in stream bytes**: streams keep
  4-byte ROM identities; resolution happens only inside
  `HostResolveGbaAddr`.

## 6. Internal graph validation — the offline song walker

A walker that parses each song's track streams with the engine's own
dispatch semantics and validates every address-bearing operand. The
prototype below was run against the retail ROM and passed 530/530.

**Dispatch model (from MPlayDef.s + the engine):**

- Bytes 0x80–0xB0: wait commands (0 operands).
- 0xB1 FINE (0) · 0xB2 GOTO (4-byte addr) · 0xB3 PATT (4-byte addr) ·
  0xB4 PEND (0) · 0xB5 REPT (1 + 4-byte addr) · 0xB6–0xB8 (0) ·
  0xB9 MEMACC (op + 1-byte index + 1-byte data; ops 6–17 additionally
  carry a 4-byte addr) · 0xBA–0xBC, 0xBD–0xC1, 0xC3–0xC5, 0xC8, 0xCC
  (1-byte operand each) · 0xC2 LFOS (2) · 0xC6–0xC7, 0xC9–0xCB, 0xCE
  (0) · 0xCD XCMD (sub byte + per-sub operands: 0x01 xwave = 4-byte
  addr; 0x00/0x03 = 0; 0x0C = 2; 0x0D = 4; else 1) · 0xCF–0xFF notes
  (0–3 consecutive operand bytes each < 0x80).
- **Running status** (music_player.c:390-399, mandatory for correct
  parsing): a byte < 0x80 repeats the previous command (only commands
  ≥ 0xBD become running status; consecutive XCMD subcommands and
  compressed notes both depend on this — 0xCD stays the running status
  across `xIECV`/`xIECL` runs).
- Walk end: FINE, or a loop within the slice (looping BGM tracks GOTO
  their `_B1` label forever — the same as the GBA player).

**Address classification** for every 4-byte operand:
same-song interior label / voicegroup start (header tone) / sample
start / programmable wave start / known runtime-BSS / **invalid — no
unknown target may pass**.

**Measured counts (retail ROM, all 530 graphs):**
- 4-byte address operands in streams: **8,285** = GOTO 1,412 + PATT
  6,873 + REPT 0 + memacc-jump 0 + xwave 0; 3,906 distinct interior
  targets; **all inside their owning song's slice; zero invalid**.
- Header pointers: 2,082 part pointers, all inside the song block;
  176 distinct tone targets, all voicegroup labels in the R12-C
  exact-start set.
- XCMD: 293 total, only xIECV (0x08, 161×) and xIECL (0x09, 132×) —
  no xwave anywhere in song graphs (the engine's one xwave consumer is
  the runtime-patched cry bytecode in m4a.c:1827, a BSS structure, not
  a song resource).
- MEMACC: 1 occurrence (non-jump). Notes: 158,911.
- The R12-D gate re-runs this walker at import time (`--check`); a
  walker failure is a manifest error, not a runtime condition.

## 7. gSongTable cutover

**Leave gSongTable unchanged.** Verified row-by-row against the retail
ROM:

- The table already holds GBA-shaped 32-bit values and they already
  equal the desired logical ROM identities: all 530 real rows hold the
  530 header symbol addresses (bijection — no row misses, no song
  missed), and the 80 dummy rows hold `dummy_song_header` = 0x086b5d00.
  No regeneration, no runtime publish, no widening (`struct Song` =
  `{GbaAddr header; u16 ms; u16 me;}` = 8 B, sizeof asserted,
  m4a.c:5). Resolution moves the *data*, not the *identity*: the row's
  value resolves through `HydrateSongHeader` →
  `HostResolveGbaAddr` → the arena copy.
- The dummy rows need nothing: the compiled 4-byte zero header stays
  linked (a documented pinned exemption, architecture §2/§8), and the
  rows keep pointing at it.

## 8. Runtime consumer path (live cutover)

The full trace (verified against the tree):
`gSongTable` → `m4aSongNumStart` family (m4a.c:214-279) →
`HydrateSongHeader` (m4a.c:81-94: header bytes via
`HostResolveGbaAddr(address)`, tone via
`HostResolveGbaAddr(T1_READ_32(raw+4))`, parts via
`HostResolveGbaAddr(T1_READ_32(raw+8+i*4))`) → `HostSongHeader` →
`MP2KPlayerMain` (music_player.c:390-415: command dispatch, running
status, `cmdPtr`/`patternStack`) → `MP2K_event_goto` →
`MP2KReadAddressOperand` (4 bytes) → `HostResolveGbaAddr`.

**Production code changes required: exactly two files.**

1. `src/platform/host_memory.c` — the interval-range table + lookup
   (the ONE engine-side change, §5).
2. `src/emerald/resources/emerald_audio_compat.c` (+ header) — copy the
   530 payloads into the arena at their ROM-relative offsets (a second
   copy loop beside the leaf loop); composition gate (exactly 530 songs,
   expected keys/sizes); register the song interval into
   `HostResolveGbaAddr`'s range table; register the 530 per-song R10
   ranges (§9); extend Republish/Clear lifecycle to the song set.

No changes to m4a.c, music_player.c, sound.c, gSongTable, gMPlayTable,
MPlayDef, songs.h. **No MP2K engine rewrite** — the engine already
resolves every song byte through `HostResolveGbaAddr`; R12-D only makes
the song span resolvable.

## 9. State-v5 implications

**Current R12-C coverage is NOT sufficient for R12-D; the extension is
the same pattern R12-C already proved.**

- Today the arena registers 198 ranges (1 whole-leaf-zone CANONICAL +
  197 transform COMPAT_OBJECT) + 1 hull. The song sub-range is unbacked,
  so no live pointer can point into it yet. After R12-D the live set
  inside GAME_BSS includes:
  - `sHostSongHeaders[0..3].parts[]` (up to 4×10 pointers → song zone)
    — NEW territory; `.tone` stays covered by the R12-C transform ranges;
  - `gMPlayTrack[].cmdPtr` (up to 23 active tracks; mid-stream interior
    addresses) — NEW;
  - `gMPlayTrack[].patternStack[0..2]` (PATT return addresses) — NEW.
- **Registration**: 530 per-song CANONICAL ranges (type
  `MUSIC_SEQUENCE`, schema 1, key `emerald:audio/song/<canonical>`,
  base = arena slice start, length = slice size) — one range per
  resource, exactly the architecture's §6 design. Any interior
  `cmdPtr`/`patternStack` address hits its song's range; the sidecar
  records identity (per-song key) + offset; load re-derives via
  `ResolveByKey` into the fresh arena. The hull (already covering the
  whole allocation) guarantees fail-closed capture.
- **Capacity**: R10 index ≈ 4,518 (R11-D) + 198 (R12-C) + 530 =
  ≈ 5,246 < 8,192 (`EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES`) ✓.
  Sidecar records per mid-play save: ≈ 6 existing + ≤ 40 parts +
  ≤ 23 cmdPtr + ≤ 69 patternStack ≪ 4,096 ✓.
- **What already works from R12-C**: the registration bookkeeping
  (verify/truncate/re-register, unregister-before-free, Republish
  hard-fail), the walker capture/restore, the post-load ordering, TEST A
  machinery — R12-D reuses all of it unchanged.
- **What still waits for R12-F**: per-leaf sub-spans for the leaf zone,
  transformed-zone per-row identity, the full R12-F lifecycle work
  (unchanged; R12-D does not expand into full per-resource sidecar
  registration beyond the song ranges, which the plan already requires
  for correctness).

## 10. Republish / relocation

- **Arena swap** (`TryInitialize` under a relocated session) already
  rebuilds the arena atomically; the song zone is part of the same
  allocation, so it moves with it. R12-D extends the existing
  clear-then-repopulate bookkeeping to the song range (host_memory) and
  the 530 song ranges (R10 index) — the exact R12-C pattern.
- **`EmeraldAudioCompat_Republish` is sufficient** — with one addition:
  it must also re-register the song interval with the CURRENT arena base
  (it already clears + repopulates the 197 exact-start entries; the
  interval joins that repopulation). No separate rehydration step is
  needed: the arena bytes are identical across republishes; the restored
  `parts[]`/`cmdPtr`/`patternStack` pointers are re-derived by sidecar
  `ResolveByKey` (pointer restore, native_state.c:3030) into the
  current arena before the republish runs.
- **Post-load ordering** (native_state.c:3061-3083) stays as-is:
  pointer restore → trainer Republish → **audio Republish** (re-registers
  exact-start + song interval + all R10 ranges; hard-fail →
  ClearMigratedEntries → compiled fallback) →
  `NativeWorldNeighborhood_Invalidate`. Active playback continues from
  the restored mid-stream positions against identical bytes.

## 11. Compiled fallback / removal boundary

- **Compiled song objects remain linked through R12-D** — they are the
  parity baseline and the additive fallback (an unresolved song address
  serves the compiled copy; both are byte-identical). The
  `--native-audio-self-test` and the battery assert the resolved
  pointers land inside the arena (song zone), proving the migrated path
  is the live one.
- **Removal stays in R12-G** (architecture §11/§8): no Makefile_pc
  change, no song-object drop, no `song_table_native.generated.h`
  re-emission in R12-D. The architecture's R12-D rollback line ("R7A
  removal is the last commit of the stage") is superseded by the R12-G
  stage boundary — the compiled songs are the parity reference until the
  final isolation stage; there is no strong reason to move removal
  earlier.

## 12. PCM parity test plan

Determinism argument first: arena song bytes == compiled song bytes
(byte gate) → identical command streams → identical resolved pointers →
the mixer is deterministic → PCM-identical by construction. The tests
then pin the behaviors that matter:

- **Offline byte gate**: whole-block memcmp of the 684,052-byte song
  zone vs the compiled binary's song bytes; per-song memcmp (pack
  payload == arena bytes == ROM slice == ELF slice) — the three-way gate
  extended to songs.
- **Looping BGM**: `mus_route101` (GOTO-loop `_B1`) plays ≥ 2 loop
  periods; resolved `cmdPtr` re-enters the arena's loop label.
- **BGM change / restart**: `m4aSongNumStartOrChange` mus_route101 →
  mus_b_frontier at fixed frame counts; same-song re-start continues.
- **SFX**: `se_use_item` (running-status XCMD + compressed notes —
  the encoding-exercising case), `se_ball_bounce_1` (2-track PATT).
- **Fanfare**: `mus_level_up`.
- **Phoneme/Bard**: `ph_nurse_held` (9-track) + Bard edit path.
- **PATT-heavy**: top-PATT songs from the gate's own counts (the
  highest-PATT graphs, e.g. the battle-frontier family) — exercises
  `patternStack` depth.
- **GOTO loop**: `mus_route101` (above).
- **xwave**: NOT present in song graphs (0 occurrences — the only
  xwave is the runtime-patched cry bytecode, m4a.c:1827); substitute
  the cry path (forward + reverse) as the xwave consumer test.
- **Battle music**: `mus_battle_wild` (10-track BGM).
- **Cry over BGM**: cry on both slots while BGM plays.
- **Save/load mid-BGM**: capture with live `parts[]`/`cmdPtr`; sidecar
  records carry song keys; fresh process re-derives into the new arena;
  playback continues PCM-identical.
- **Save/load inside a pattern**: mid-PATT save (a patternStack depth
  ≥ 1 at capture — pick a PATT-heavy song, save at a fixed frame inside
  the pattern region); restored `patternStack`/`cmdPtr` re-derived.
- All parity assertions run against the compiled baseline (the R12-B/C
  harness pattern: real SDL2 device, fixed frame counts, byte-compared
  mixer output), plus the relocation/republish variants.

## 13. Failure matrix (fail-closed everywhere; no mixer crash acceptable)

| Failure | Behavior |
|---|---|
| missing song resource | session refuses transactionally (composition gate: exactly 530, expected keys) — arena absent, compiled songs serve |
| wrong type / schema | refuse at publish (per-entry type/schema check, the leaf-loop pattern) |
| truncated header / bad trackCount | walker + header validation refuse at import; publish-time size check (8+4·N == payload size) refuses |
| invalid track count (N > player's track capacity) | walker refuses (N outside 0..10) |
| out-of-bounds track pointer (part outside slice) | walker refuses (0 occurrences today — the gate keeps it true) |
| GOTO/PATT target outside the owning slice | walker refuses (0 occurrences today) |
| unknown voicegroup tone target | publish refuses (tone must hit the 197-label exact-start set) |
| corrupt xwave target | walker refuses (n/a in current graphs; gate ready) |
| duplicate logical range / label | `HostMemoryRegisterLogicalRange`/exact-start registration aborts on duplicates (sorted insert, the R12-C pattern) |
| partial publication failure | transactional arena build — nothing publishes until all 530 validate and copy; failure → arena absent → compiled fallback |

Every refusal names the first failing resource in the seam diagnostics;
none can reach the mixer.

## 14. Pack / provenance

- **Pack delta: 5,289 + 530 = 5,819 entries.**
- **Payload delta: 684,052 B (0xA7014)** = mus 651,184 + se 31,304 +
  ph 1,564. Pack size: current 8,854,624 B → **≈ 9.59 MiB**
  (+684,052 payload + ~530×≈100 B key/index/name overhead ≈
  9,575,000–9,625,000 B; exact value generator-pinned).
- **Manifest**: +530 records (id, key, type `music-sequence`, schema 1,
  symbol, rom_offset = slice start, length = slice size);
  **catalog**: +530 rows; **bindings**: per-song slice + trackCount +
  walker validation transcript.
- **Provenance chain** (the R9-R11 proof, extended): tracked `.s` /
  `.mid`+mid2agb source == qualified ELF slice == retail ROM slice ==
  pack payload == arena verbatim bytes. Re-proven at `--check` time by
  `gen3-elf-manifest` (symbols + walker) and `gen3-pack-build`
  (determinism), and at publish time by the seam's per-payload memcmp.

## 15. Performance

- Hot path: `HostResolveGbaAddr` per GOTO/PATT event and per song
  start (hydration). Current cost: binary search over 197 exact-start
  entries (~8 comparisons) + handle check + cast. R12-D adds one
  interval range: +1 bounds check + ~1 comparison + one add. Total
  ~10–15 ns — negligible against per-frame mixer work; address
  resolution happens per note-event, not per sample.
- Table sizes: 197 exact-start (R12-C) + ≤ 16 intervals (1 used in
  R12-D). No allocation on the hot path (registration is publish-time
  only, fixed capacity).
- No credible regression risk for GOTO/PATT-heavy playback or
  overlapping SFX/BGM (four music players resolve through the same
  read-only table). Flagged as an explicit STOP condition anyway:
  STOP if the self-test/battery shows any perf or determinism delta
  (architecture §11 stop condition — unchanged).

## 16. Implementation staging for Flash (stop gates between steps)

A. **Generator**: emit 530 song bindings (slice start/size/trackCount
   from the packed-object rule §2) + the stream walker (§6) as an
   import-time validator; `gen_audio_bindings.py` extension +
   walker module. Gate: walker 530/530 on the reference ROM;
   three-way byte equality; manifest `--check`.
B. **Pack**: import 530 resources → manifest 771→1,301 audio entries,
   pack 5,289→5,819. Gate: `gen3-pack-build --check` byte-identical;
   production-proof count updates.
C. **Arena**: seam copies the 530 payloads at ROM-relative offsets
   into the existing verbatim zone; composition gate; per-payload
   memcmp vs pack. Gate: leaf offsets unchanged (R12-B battery);
   published song bytes == pack (extended leaf-runner F).
D. **Logical range**: `HostMemoryRegisterLogicalRange` +
   `HostResolveGbaAddr` interval lookup; seam registers the song span
   on publish/republish, clears on Clear. **FIRST LIVE BEHAVIOR
   CHANGE** — from here, song starts hydrate into the arena (bytes
   identical to compiled). Gate: runtime resolution test — hydrated
   `parts[]` ∈ arena song zone; GOTO loop resolves to the arena's
   `_B1`.
E. **State-v5 forward-pull**: 530 per-song CANONICAL R10 ranges +
   lifecycle; extended TEST A (mid-BGM/mid-pattern save/load,
   cross-restart, ResolveByKey into the fresh arena); fingerprint gate.
F. **PCM/regression battery**: full 35-runner battery + extended
   `--native-audio-self-test` (§12 matrix) with arena-resident
   assertions; guardrail 18 re-check (the walker/generator stay in
   tools/, the seam stays platform-neutral — the weak-extern
   HostMemory hooks from R12-C already satisfy this).
G. **Report**: `docs/R12D_SONG_GRAPH_MIGRATION_REPORT.md`; no commit
   until the stage gate is green.

Steps D/E are the only steps with behavior-visible effect; C is
additive-only (bytes copied, nothing resolves to them yet).

## 17. Required findings (summary)

1. **Songs**: 530 = mus 210 + se 269 + ph 51; 684,052 B canonical
   (mus 651,184 / se 31,304 / ph 1,564); min 8 B (mus_dummy), max
   16,396 B (mus_rg_credits); track counts 0–10 (distribution §1);
   80 dummy rows → compiled `dummy_song_header`, NOT resources.
2. **Slice derivation**: header symbols + trackCount byte + proven
   packed-object invariant (0 mismatches across 530 objects / 7,740
   labels) + independent anchors (leaf tail 0x088FC03A → block start
   0x088FC03C; Sio32 data → block end 0x089A3050) + walker + three-way
   gate. Not the fragile next-symbol heuristic.
3. **Payloads**: verbatim GBA-form; 4-byte address operands stay;
   no host pointers, no widening.
4. **Arena**: songs already inside the existing span — no resize, no
   rebase; zero collisions; leaf and transform offsets unchanged.
5. **Resolution**: hybrid — exact-start (197) + ONE interval for the
   contiguous song block; precedence exact-start → interval → handle →
   direct cast; no host addresses in streams.
6. **Walker**: engine-accurate dispatch (running status + variable
   notes + XCMD subs); 8,285 address operands (1,412 GOTO + 6,873
   PATT), 3,906 distinct targets, all interior, zero invalid; 0 xwave
   in song graphs; 176 distinct tone voicegroups; 2,082 parts.
7. **gSongTable**: unchanged — values already equal the logical ROM
   identities (verified row-by-row); no widening.
8. **Cutover**: two files change (host_memory.c range table;
   emerald_audio_compat.c song zone + registration). No MP2K rewrite.
9. **State-v5**: R12-C coverage insufficient by itself; add 530
   per-song CANONICAL ranges (index ≈ 5,246 < 8,192); sidecars ≪
   4,096; full R12-F work still waits.
10. **Republish**: sufficient, extended to the song interval + song
    ranges; no extra rehydration step; ordering unchanged.
11. **Fallback**: compiled songs stay linked through R12-D; removal
    remains R12-G.
12. **Pack**: 5,819 entries; +684,052 B; ≈ 9.59 MiB; four-way
    provenance chain.
13. **Perf**: O(log 197) + O(1) interval — no credible regression;
    perf stop condition retained.
14. **Highest risk**: walker fidelity vs. the engine's running-status
    semantics (silent-wrong import risk — mitigated by the three-way
    byte gate + the engine-as-reference argument), then state-v5 song
    range completeness (loud, fail-closed), then resolution precedence
    (behavioral, additive-degrade).
15. **Split**: not recommended — the pack → walker → arena → range →
    registration chain is one transactional unit (the R12-C C1/C2
    reasoning applies); steps A–C are additive and individually
    gateable if review pressure demands a smaller first PR.

### STOP conditions (per the stage brief)

- If any song lies outside the current span → STOP. **Not hit** (all
  inside; §4).
- If the walker cannot validate all 530 graphs (any unresolved/unknown
  target) → STOP. **Not hit** (prototype: 530/530, 0 invalid).
- If any PCM/perf delta appears at the first live step → STOP and
  revert to compiled-only resolution.
- No commit until the full battery is green; no R12-E work.
