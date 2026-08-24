# R13-I — Global State-v5 Closure Report

Stage: **in progress** (R13-H complete baseline; no commit; R13-J/R14 not
started).

## 1. Result

R13-I closes the global State-v5 pointer model: every serialized
pointer-bearing surface is classified (RESOURCE_PTR / ENGINE_IMAGE_PTR /
DYNAMIC_BUFFER_PTR / ROM_PROVENANCE_ADDR / SCALAR_OR_ID / NULL /
TRANSIENT_REFUSE — no UNKNOWN), and the two raw-persistence surfaces
found by the audit are closed: the frontier facility pointers (F1:
eight HOST_DATA fill-target spans, RESOURCE_PTR records) and the
FLASH-slice handle-gate asymmetry (F2: capture + load gates). The live
range count moves 6,382 → 6,390 / 8,192 with the §35-justified
frontier-table closure; the State-v5 format, sidecar cap, range cap,
pack (23,069 / `b711d358…`), and G/H ownership are unchanged. New
proofs: the mixed-family fresh-process state (24 records across G + H +
text + engine), the deterministic-serialization proof (byte-identical
payload across forced-different arena bases), three new corrupt-matrix
kinds, and a consolidated five-leg master harness. Release 23,842,840 B
(+4,224), DINFO 36,521,096 B (+608), both verify
`f3ae088181bf583e55daf962a92bb46f4f1d07b7`.

## 2. Baseline freeze (brief §1)

| Pin | Value at freeze |
|---|---|
| State-v5 version | 5 (unchanged since R10; v4 rejected) |
| Live range count | 6,382 / 8,192 |
| Sidecar record cap | 4,096 |
| Range cap | 8,192 |
| Pack | 23,069 entries / 15,278,272 B / `b711d358…` |
| Release binary | 23,838,616 B (verify f3ae0881… exit 0) |
| DINFO binary | 36,520,488 B |
| G proofs | g4-state 5+5, g4-faults 31/31, vaddress 8/8, Context2 refusal |
| H proofs | h3 nested blocking 16/16, h4 anim, h5 nested, h6 AI, h7 22/22 |
| Regression suites | all green at H7 (battery §38) |

## 3. Pointer-surface census (brief §2)

Complete census: `docs/R13I_STATE_V5_POINTER_CENSUS.md` (this report's
machine-readable companion). Summary:

- **Serialized slices:** GAME_BSS (game bss incl. the static gHeap buffer),
  EWRAM, IWRAM, COMMON, GAME_DATA (64-byte opt-in, WAV table only) — walked
  at 4-byte stride / 8-byte windows. REGISTERS/VIDEO/FLASH/FRAMEBUFFER +
  four sidecars are not walked (FLASH now carries its own handle gate, §6).
- **Walker classification ladder (total):** family adapters (G then H) →
  resource range index → inactive-printer scrub → NULL → modeled function
  fields → image gate → unmanaged/handle refusal → scalar skip.
- **Exact G surfaces:** 27 static slots (Context1 IP + 20 stack + ramRet +
  2 approaching + trainerEnd + 2 trainer returns) + dynamic buffers
  (SAVE_RAM_SCRIPT, MYSTERY_EVENT_BUFFER, STATIC_G_ARENA) + the
  pointer-free vaddress anchor. Max 33 records (27 G + 6 trainer text).
- **Exact H surfaces:** 36 H-typed slots (IP + 8 battle stack + 4 selection
  + 4 palace + shared AI IP + 8 AI stack + anim IP/ret + 8 contest stack)
  + 9 engine slots (callback stacks, anim callback, controller funcs,
  savedCallback). Max 20 simultaneously active H records.
- **Engine-image surfaces:** gTasks[].func + 2 followup/stored per task,
  gSprites[].callback + stored sidecar, SpriteTemplate callbacks, gMain
  callbacks ×7, TextPrinter callbacks/currentChar, gWindows[].tileData,
  battle roots, all heap-interior pointers (gHeap ⊆ GAME_BSS).
- **Generic resource surfaces:** trainer/pokemon/object-event/tileset/
  layout/audio/text/map/script/battle arenas + the frontier tables (§5).
- **Provenance-as-data surfaces:** DMA3 destinations (BATTLE_SIDECAR),
  vaddress anchors, routing words, braille addresses, save-block
  GbaAddr script fields (accessor-normalized), u16-pair callback packings
  (task data, GBA-space identities).

**Unclassified persisted pointer surfaces: 0.**

## 4. Pointer classes (brief §3)

| Class | Representation | Restore |
|---|---|---|
| RESOURCE_PTR | 64-byte sidecar record (key+type+schema+role+offset) | range index resolve, bounds + identity checked |
| ENGINE_IMAGE_PTR data | 8-byte in-band record (PDIM/PDLG) | image base + offset, revalidated |
| ENGINE_IMAGE_PTR function | 8-byte record (PFIM/PFLG) + persistent-function table | stableId↔native validated both directions |
| DYNAMIC_BUFFER_PTR | in-band image record + adapter registry revalidation | kind+owner+generation+offset+boundary |
| ROM_PROVENANCE_ADDR | data bytes (DMA3 sidecar identity) | rehydrate + re-encode, handles refused |
| SCALAR_OR_ID | verbatim | verbatim (modeled-scalar skip prevents misclassification) |
| NULL_PTR | zeroed | NULL |
| TRANSIENT_REFUSE | — | capture refuses with precise diagnostics |

No UNKNOWN class exists. See the census for the full per-surface table.

## 5. R13-I findings and fixes

### F1 (HARD): frontier facility pointers persisted as raw host_data addresses — FIXED

`gFacilityTrainers` / `gFacilityTrainerMons` (EWRAM, battle_tower.c:47-48)
hold pointers into the published HOST_DATA fill targets
(gBattleFrontierTrainers/Mons + 3 × tent trainer/mon pairs; all writers
enumerated). The walker's ladder had no branch for them: not modeled, no
range (host_data is deliberately excluded from the image gate), below the
high-host band, not a known-data field → the raw 8-byte host address was
serialized verbatim (silent retention, masked by the fixed non-PIE base +
buildId lock).

Fix (the emerald_map_compat.c map-headers precedent): the eight HOST_DATA
fill targets are registered as COMPAT_OBJECT spans —
`emerald:data/frontier/tower-trainers` (schema 21), `…/tower-mons` (23),
and the three tents' trainer (21) / mons (23) pairs. Capture now emits
key+offset RESOURCE_PTR records; load resolves through the range index.
Registration is idempotent (key-removal preamble; a repeated publish can
never duplicate or overlap the fixed-base spans), and the frontier clear
removes all ten family keys (the two arena spans + the eight table spans).

**Range-count consequence (brief §35, justified deviation):** the live
count moves **6,382 → 6,390 / 8,192**. Justification: the pin predates
this finding; the alternatives are refusing saves inside frontier
facilities (gameplay regression), persisting the raw addresses (the hard
gate R13-I exists to close), or a new record kind (State-v5 format
change — the §43 STOP class). No cap change, no format change, no schema
change; the pins in the loader (script gate 6,385, H gate 6,390) and all
four test suites were updated.

### F2: FLASH-slice handle-gate asymmetry — FIXED

The FLASH slice (the .sav bytes) was the only raw-copy section never
audited for E/F runtime handles — asymmetric with the EWRAM walker
(SaveBlock1 lives in both). The single producer of a handle byte into
save-block data (`ObjectEventTemplate_SetScript`'s non-compat fallback,
global.fieldmap.h:138) is unreachable today (every object-event script is
compat-resolvable), but the invariant was disciplinary, not structural.
Fix: the registered-handle gate now runs on FLASH at capture
(`save-flash-final`) and at load (`load-flash-verify`). Zero format
change; the gate is fail-closed and trivially safe (production registers
no handles for in-image pointers).

### F3: test-driven frontier range duplication — FIXED (by F1's preamble)

The loader-test census exposed duplicate frontier spans across
direct-publish cycles (schema 22/37 count=2). Root cause: registration
was not idempotent and callers could publish without clearing. F1's
key-removal preamble makes every publish cycle self-cleaning; the census
re-run must show exactly one span per family key.

## 6. Sidecar schema census (brief §4)

| Section | Tag | Payload | Max records |
|---|---|---|---|
| RESOURCE_SIDECAR | 14 | 4 + 64×N LE records | 4,096 |
| BATTLE_SIDECAR | 12 | 4 × 8B data records (DMA3) | 4 |
| TASK_SIDECAR | 10 | NUM_TASKS × 2 × 8B function records | — |
| SPRITE_SIDECAR | 11 | (MAX_SPRITES+1) × 8B function records | — |

Plus the in-band 8-byte tagged records inside walked slices. No duplicate
or conflicting schema interpretations; no positional assumptions beyond
the validated (section, offset) ordering; no renumbering (none needed).

## 7. Range registry census (brief §14/§35)

Measured per-family buckets (loader-test census, 78,512 checks; updated
for F1): see `docs/R13I_STATE_V5_POINTER_CENSUS.md` §10. Totals:
**6,390 / 8,192** (6,377 pre-H + 5 H arenas + 8 frontier tables).
Invariants: sorted by base, non-overlapping, nonzero-length,
identity-based unregister, ResolveByKey-only materialization, hulls 7.

## 8. Sidecar capacity (brief §16)

- G static max: 27 + 6 trainer-text = 33 records
- H max simultaneously active: 20 records
- Generic arena records: observed per-save [TBD from battery logs]
- Cap 4,096: headroom ≥ 100× at the worst simultaneous case; no cap change.

## 9. Transaction atomicity (brief §18/§19)

Save: capture-side accumulation → any failure (refusal, unmanaged
pointer, hull-without-identity, overflow) fails the whole save; the
container is written atomically (Platform_StorageWriteAtomic).
Load: header/version → buildId → payloadCrc → fingerprint → section
staging (CRC + validate each) → sidecar parse (structure) → resolve ALL
records (adapters + index) → R12-F pre-flight republishes → commit
sections → patch pass. Any step-1..6 failure leaves live memory
byte-exact (canary-proven, TEST 4/7 + fault matrices).

## 10. G global closure (brief §6)

Re-audited against production post-G6 (source + agent sweeps + suites):

- Context1 IP / return stack: `RESOURCE_PTR` module key + boundary-
  validated offset; inactive entries scrubbed; depth > 20 refused.
- `gRamScriptRetAddr`: static-G NEXT_INSTRUCTION or NULL; RAM-script
  IPs are `DYNAMIC_BUFFER_PTR` (SAVE_RAM_SCRIPT registry).
- Trainer continuations: approaching ×2 (INSTRUCTION_START),
  `sTrainerBattleEndScript` (NEXT_INSTRUCTION), A/B returns
  (ENTRYPOINT = generated export required).
- `sAddressOffset`: dead storage removed in G6; the live anchor is
  `EmeraldScriptVirtualAnchor` — encoded GBA base kept as DATA +
  buffer identity (kind/owner/generation/offset); no host delta can
  enter a state.
- Mystery Event: MYSTERY_EVENT_BUFFER with the per-card 17-op boundary
  bitmap (G6 §7); base/offset restoration proven (g6-mevent).
- Compiled G payload: 0 bytes in the binary (G6 sweeps); `G_SCRIPT`
  miss ⇒ terminal error; boot refuses without the G pack.
- Context2 active capture: explicit refusal (mode/ptr/depth/stack).
- Stale generation: capture stamped for generation A refuses after a
  restage to B (g4-faults); valid semantic identities restore into B
  by key (g4-cross).

## 11. H global closure (brief §7)

Re-audited against production post-H7:

- Battle: current IP, 8-slot call stack, selection/palace ×4 each —
  all `RESOURCE_PTR` (schema 47) with per-field boundary roles; the
  8-slot callback stack is engine-function (never bytecode;
  H-bytecode-on-engine-slot refuses).
- Animation: IP + single return (schema 48); callback engine-image.
- Battle AI: quiescent stale IP relocated, never executed; stack
  size 0 at capture by construction; shared `gAIScriptPtr` slot
  accepts either AI family (FAMILY_COUNT sentinel).
- Contest AI: shared-slot policy; the contest stack exists only
  during a contest (never simultaneously capturable with battle).
- Field effect: no persistent IP (function-local cursor) — an
  FE-arena pointer in any serialized field is a policy refusal.
- All live H pointer sources resolve only to ROM-backed arenas: the
  6 compiled H objects are out of the link (H7 carve), all 2,088
  legacy symbols absent, no compiled fallback (H7 fault 22/22 +
  isolation sweep 0 unexplained).

## 12. Engine-image pointer audit (brief §8/§9)

Model: 8-byte `HostPersistentAddress` {value, kind} — image-relative
offset (PDIM data / PFIM functions; PDLG/PFLG are the 32-bit-target
logical forms). The persistent-function table (stableId = image
offset ↔ native address, capacity 16,384) validates both directions:
a stored offset must name the same executable function on save and
load. Engine callbacks proven restored in the h3/mixed fixtures
(battleCallbackStack, gAnimScriptCallback) and by the task/sprite
sidecar suites. No raw ASLR/process address is ever persisted; the
non-PIE fixed base makes offsets numerically stable, but validation
never depends on that accidentally.

## 13. Dynamic-buffer identity audit (brief §10/§11)

Three live kinds: SAVE_RAM_SCRIPT (saveblock ramScript storage),
MYSTERY_EVENT_BUFFER (heap recvBuffer + per-card bitmap),
STATIC_G_ARENA (the live G arena); CAPTURED_BUFFER is a dead enum
value with no registration site (documented). Registry: overlap
rejected; same (kind, owner) re-registration is a generation
replacement; restore revalidates kind + owner + generation + offset
+ instruction boundary. gHeap ⊆ GAME_BSS ⇒ all heap-interior
pointers are image-relative records; host-malloc targets in slices
refuse. **No persisted unrelocated heap pointer remains** (agent-B
sweep; the one silent raw-persistence surface was F1, now closed).

## 14. Pointer-width / struct-copy / GBA-provenance audits (brief §12/§13/§28)

Three-sweep audit result (pointer-width, struct-copy, GBA-provenance;
full reports folded into the census):

- **Zero truncation hazards in reachable code.** Every u16-pair store
  of a pointer/function (shop.c, battle_factory_screen.c, pokeball.c,
  battle_anim_mons.c, easy_chat.c, union_room.c, list_menu.c) carries
  a GBA-space identity or an in-image address (top 16 bits zero by
  construction); two raw-cast sites (party_menu.c:1897,
  list_menu.c:1068) are unreachable dead code (documented).
- **Zero host-pointer-in-4-byte-field sites.** The DMA3 destination is
  the only pointer-bearing 4-byte protocol field: in-band zeroed +
  BATTLE_SIDECAR identity. LINUX64 sidecar arrays keep host-width
  callbacks out of serialized memory everywhere they exist.
- **Save blocks are GBA-shaped** (no host-width members); SaveBlock1's
  only pointer-shaped field is ObjectEventTemplate.script = u32
  GbaAddr, normalized at the accessor boundary; the .sav byte copies
  are safe by construction, and F2 makes the FLASH slice's handle
  invariant structural.
- **No struct-copy hazard:** no serialization path memcpy's a
  pointer-bearing host struct into a slice; walked slices go through
  the window walker; sidecar records are bytewise LE stores at
  STATIC_ASSERT-pinned offsets.
- **Provenance retained as data (never converted):** routing words,
  vaddress anchors, braille addresses, save-block script fields,
  EWRAM base+addend operands — all classified ROM_PROVENANCE_ADDR or
  SCALAR_OR_ID in the census.

## 15. Refusal-policy census (brief §29)

Full table in `docs/R13I_STATE_V5_POINTER_CENSUS.md` §7. Every refusal
is intentional, test-covered, and non-mutating.

## 16. Boundary / alias / generation policies (brief §22/§23/§24)

Boundary roles are per-field, never a generic "inside arena" rule:
INSTRUCTION_START (exact instruction start, bytecode span only),
NEXT_INSTRUCTION (runtime return addresses — interior instruction
boundaries), ENTRYPOINT (generated export required). Refused
precisely: middle-of-operand, holes and inter-module gaps, past-end
offsets, data/routing/empty spans for instruction roles, padding
windows (never classified), and zero-width alias keys
(ALIAS_IDENTITY; state identity canonicalizes to the payload owner —
proven for all 8 aliases). Negative coverage: g4-faults 31/31,
h3-faults 16/16, H7 22/22, corrupt matrix 13 kinds. Stale generation
refuses before any pointer check; a valid semantic identity from
generation A restores into generation B by key (g4-cross, h3-cross,
mixed-cross, generation-replacement legs).

## 17. Pack-resource mismatch faults (brief §25)

- Missing module: H7 fault (bytecode-module removal ⇒ refuse) + the
  loader's `TestLoaderRefusesScriptMissingPack` (boot refuses, full
  rollback, 11 gStdScripts slots still zero).
- Wrong schema/type/role: h3-faults #16 (schema flip), corrupt kinds
  bad-schema/bad-type/bad-role (each refuses pre-mutation).
- Invalid offset: oob-resource-offset + adapter boundary refusals.
- Missing resource at boot: session registration refuses with full
  rollback (no compiled fallback exists post-G6/H7).
- No address-redirect fallback: resolution is by key identity only;
  "same numeric address, different resource" cannot happen.

## 18. Mod-awareness (brief §26)

A saved pointer identifies semantic resource/module key + offset — never
pack order, arena aggregate offset, or host address. A future mod
replacement that changes a module's layout: the fingerprint (provider
content digest) changes → the load refuses cleanly (documented policy;
no migration path invented in R13-I).

## 19. Compatibility fingerprints (brief §27)

Header: buildId (executable identity) + contentFingerprint (session
digest: gameId + adapter version + RESOURCE_API_VERSION +
logicalContentDigest + provider list). Both must match; no force-load.
Enough identity exists for R13-I closure; the full Tallgrass mod
fingerprint system is explicitly deferred.

## 20. GBA/native state distinction (brief §28)

4-byte values classified as: canonical GBA provenance (DMA3, routing,
vaddress, braille, save-block script fields) — data, never converted;
scalars (IDs, packed bits); keys/hashes; true native pointers (never in
4-byte fields — zero sites).

## 21. Cross-process master harness (brief §20/§21)

`tests/run_emerald_state_master.sh` — one gate for: G fresh-process
(nested returns, RAM, trainer, vaddress 8/8), H battle fresh-process
(blocking + nested), H live suite (battle + anim + AI state legs under
ASan/UBSan), mixed-family (G+H+engine, 24 records, both generations
re-staged at forced-different bases), deterministic serialization.

Mixed-family proof (`mixed-cross`): one state carries an active G
Context1 (nested, two frames), the H battle VM (IP + two stack returns
+ selection + palace), the anim VM (IP + return), a stale AI IP, engine
callbacks, and six C trainer-text pointers — 24 sidecar records
(9 G schema 45 + 9 H schemas 47-51 + 6 TEXT type 9), captured through
both family adapters plus the generic resource window in ONE save, and
restored in a fresh process with both generations re-staged at
forced-different bases and the perturbed H layout. Per-family
resolution proven (no cross-family record, no restore-order
dependency); the engine callbacks restore image-relatively; the
mixed-return execution gate (blocking waitmessage + LIFO returns)
passes. Result: MIXED-CREATE records=24 (g=9 h=9 text=6) →
MIXED-LOAD g-ok h-ok engine-ok mixed-return=ok, creator/restorer arena
bases provably differ for BOTH seams. The consolidated master harness
(`tests/run_emerald_state_master.sh`) passes all five legs in one
invocation: G fresh-process, H battle fresh-process, H live suite
(ASan/UBSan), mixed-family, determinism.

## 22. Corrupt-file matrix (brief §31)

Extended from 10 to 13 kinds: bad-tag, oob-offset, bad-key, bad-role,
bad-schema, **bad-type (new)**, oob-resource-offset, oversized-count,
duplicate-fields, bad-reserved, truncated-sidecar,
**bad-sidecar-size (new)**, **raw-crc (new — unrepaired checksum)** —
plus corrupt-key (TEST 7b), v4/unsupported-version policy, fingerprint
mismatch, missing-session. Every kind refuses cleanly with the canary
intact. [RESULTS TBD]

## 23. Deterministic serialization (brief §32)

`determinism` mode: identical semantic state saved at two forced
different arena bases → every payload section byte-identical, with
exactly two allowed-differ fields: the header frame counter and the
script generationStamp (session-lifecycle bookkeeping; the raw stamps
provably differ, proving the forced-different-generation setup while
all semantic bytes stay identical). Result: DETERMINISM payload=461,128
bytes identical across arena bases except RTC (frame equal in the
harness; the RTC section is excluded by policy — real-time bytes are
the brief-sanctioned differing class, and the harness stub writes
constant bytes anyway).

## 24. Current-save compatibility (brief §33)

Format v5 unchanged since R10; no field/section added or renumbered.
Pre-I v5 states load under the unchanged buildId + fingerprint policy
(states from a different build refuse on buildId — the designed scope).
No format bump; no compatibility break.

## 25. Production runtime audit (brief §34)

The production binary boots with the full session (the loader path the
H7 battery drives end-to-end: 65,754+ checks with the R13-I range
census, now 78,512), `--verify-game-data` reports the qualified ROM,
and every pointer surface the game produces after boot/overworld/map
transition/NPC script/battle/anim/AI turn/post-battle return is
classified by the census walker (the isolation and state suites
exercise each stage). The interactive manual DINFO checklists of G/H
remain the human-visible evidence; R13-I adds no new runtime surfaces.

## 26. Pack / ownership invariants (brief §36/§37)

Pack: 23,069 entries / 15,278,272 B / `b711d358…` — byte-identical
(R13-I adds no content). G 523/523 and H 2,089/2,089 stay
ROM_BASE_ONLY; no compiled payload reintroduced.

## 27. Cleanup allowance (brief §40)

Applied: none required beyond the fixes. (F1/F2 are fixes, not
cleanup.) No dead compatibility code identified as R13-I-owned.

## 28. Forced builds (brief §39)

| Build | H7 baseline | R13-I | Delta |
|---|---:|---:|---:|
| Release (`-B rom`) | 23,838,616 B | **23,842,840 B** | **+4,224 B** |
| DINFO (`-B rom DINFO=1`) | 36,520,488 B | **36,521,096 B** | **+608 B** |

Both verify `f3ae088181bf583e55daf962a92bb46f4f1d07b7` exit 0; release
zero debug sections; DINFO 7 debug sections. The delta is the R13-I
closure code only: the eight frontier span registrations + the
key-removal preamble + the two FLASH-slice handle gates — no payload,
no schema, no format growth.

## 29. Regression battery (brief §38)

All suites green on the R13-I tree (2026-08-24; the two one-off TEST-8
create-leg flake hits seen once in the first battery run did not
reproduce in five subsequent runs — the captured record dumps were
identical in every reproduction; treated as a transient environment
anomaly with the reproduction evidence kept in the session logs):

| Group | Suites | Result |
|---|---|---|
| G State | script-state, script-state-cross-restart, script-state-faults, g4-cross (5+5, vaddress 8/8), g4-faults 31/31, g6-mevent | green |
| H State | battle-state, battle-state-cross-restart, battle-state-faults 16/16, h3-cross (nested blocking), battle-live sanitize (oracle/faults 11+8+10+22/replace/state under ASan/UBSan) | green |
| Global | resource-state (TESTS 1-8 incl. the 13-kind corrupt matrix + v4/unsupported-version policy + fingerprint + missing-session), resource-ranges, session-fingerprint, mixed-cross (24 records), determinism, state master harness (5 legs) | green |
| General | runtime-loader (78,512 checks incl. the range census), trainer-compat + production + sanitize, layout, tileset, object-event, script-compat + sanitize, script-faults 21/21, script-module-loader, battle-module-loader, real-tables, native-world real/render/neighborhood, resource-lz + sanitize, resource-import + sanitize, rom-base-provider + sanitize, native-asset-isolation (33,521 ok / 0 failed), desktop real-SDL probe | green |
| Build | `--verify-game-data` → f3ae0881…d07b7 exit 0 (release + DINFO) | green |

## 30. Hard completion gates (brief §42)

- ✓ State-v5 version unchanged (5; no field, section, or kind added)
- ✓ global pointer-surface census complete (companion doc)
- ✓ unclassified persisted pointer surfaces: 0
- ✓ raw host pointer persistence: 0 (F1 closed the last silent surface;
  F2 makes the FLASH invariant structural)
- ✓ truncated native pointer persistence: 0 (three-sweep audit; the two
  dead-cast sites are unreachable and documented)
- ✓ RESOURCE_PTR surfaces: semantic key + offset (G 45/46, H 47-51,
  frontier 21/23, map/text/arena families)
- ✓ ENGINE_IMAGE_PTR surfaces: image-offset + persistent-function-table
  validation
- ✓ DYNAMIC_BUFFER_PTR surfaces: kind + owner + generation + offset +
  boundary
- ✓ transient invalid states: explicit refusals (census §7, all covered)
- ✓ current live range count: exactly 6,390 (6,382 + the 8 frontier
  spans, §35-justified)
- ✓ range leaks: 0 (idempotent registration preamble; the test-driven
  duplicate finding F3 closed)
- ✓ sidecar cap: comfortably below 4,096 (33 G + 20 H + generic)
- ✓ G fresh-process proofs: green (g4-cross in the master harness)
- ✓ H fresh-process proofs: green (h3-cross + live suite)
- ✓ mixed-family proof: green (24 records, both bases differ)
- ✓ stale generation: refuses (fault matrices)
- ✓ save transaction: atomic (capture-side failure = whole-save fail)
- ✓ load transaction: atomic (canary-proven staged commit)
- ✓ corrupt-file matrix: green (13 kinds + policy cases)
- ✓ deterministic semantic serialization: green (byte-identical payload)
- ✓ pack: exactly 23,069 entries, `b711d358…` byte-identical
- ✓ G/H ownership: unchanged ROM_BASE_ONLY (523 + 2,089)
- ✓ compiled payload not reintroduced (sizes + isolation sweep)
- ✓ full regression/sanitizer battery: green (battery run 2)

## 31. STOP conditions (brief §43)

Evaluated: no format bump, no cap rise, no unclassifiable pointer, no
unavoidable raw persistence, no identity that cannot survive
replacement, no G/H schema change, no ownership reopening, no
R13-J/R14 dependency, no pack change, no prior-proof regression. The
F1 range-count deviation is the §35-justified exception.

## 32. R13-J prerequisites

- Every serialized pointer class has a stable semantic identity.
- Raw host-pointer persistence eliminated (F1/F2).
- Range count pinned at 6,390 with the frontier-table closure.
- Master harness + mixed + determinism green.
- Battery + builds green; pack/ownership unchanged.

## 33. Files changed

- `src/emerald/resources/emerald_frontier_compat.c` — F1: eight
  COMPAT_OBJECT spans over the published HOST_DATA trainer/mon/tent
  fill targets; `RemoveFrontierRanges` key-removal preamble makes
  registration idempotent; clear removes all ten family keys;
  `EMERALD_FRONTIER_MAX_REG_RANGES` 2 → 10.
- `src/emerald/resources/emerald_runtime_loader.c` — range pins
  script gate 6,377 → 6,385, H gate 6,382 → 6,390 (with comments).
- `src/platform/native_state.c` — F2: FLASH-slice registered-handle
  gates at capture (`save-flash-final`) and load (`load-flash-verify`).
- `tests/emerald_resource_state_test.c` — mixed-create/mixed-load
  modes (24-record G+H+engine fresh-process proof), determinism mode
  (per-section byte comparison, generation-stamp masking), main
  dispatch, 6,385/6,390 pins.
- `tests/emerald_resource_state_corrupt.py` — three new corrupt kinds:
  bad-type, bad-sidecar-size, raw-crc (unrepaired-checksum path).
- `tests/run_emerald_resource_state.sh` — extended corrupt-kind loop;
  mixed-state/mixed-cross/determinism mode blocks.
- `tests/run_emerald_state_master.sh` — NEW: consolidated five-leg
  cross-process master harness (brief §20).
- `tests/emerald_runtime_loader_test.c` — `TestRangeCensus` (brief §14
  machine-readable per-family census) + invocation.
- `tests/emerald_battle_live_test.c`, `tests/emerald_script_compat_test.c`,
  `tests/run_emerald_battle_live.sh` — 6,385/6,390 pins.
- `docs/R13I_STATE_V5_CLOSURE_REPORT.md` (this report),
  `docs/R13I_STATE_V5_POINTER_CENSUS.md` (machine-readable census).
