# R13-I — State-v5 Global Pointer-Surface Census (working draft)

Status: draft during R13-I audit; final version folded into
`docs/R13I_STATE_V5_CLOSURE_REPORT.md`. This file is the machine-readable
census backing the report (brief §2/§3): every pointer-bearing surface that
can enter State-v5, its class, serialization mechanism, validation rule, and
fresh-process behavior.

## 1. Pointer-class taxonomy (brief §3)

| Class | Meaning | Serialized representation | Fresh-process behavior |
|---|---|---|---|
| `RESOURCE_PTR` | pointer into a registered resource arena (ROM-backed compat stream) | 64-byte sidecar record: sectionTag + fieldOffset + resourceKey[32] + type + schema + role + rangeOffset; in-band bytes zeroed | resolved against the active session's range index by key+type+schema+role, offset checked < length; pointer materialized in the current process |
| `ENGINE_IMAGE_PTR` (data) | pointer into game-image memory (game bss incl. gHeap, EWRAM, IWRAM, COMMON, static data) | 8-byte in-band `HostPersistentAddress` {u32 value, u32 kind}; kinds PDLG (logical) / PDIM (image-relative offset) | value + imageStart (non-PIE fixed base), revalidated to be game-image memory and not executable/host_data |
| `ENGINE_IMAGE_PTR` (function) | executable callback/task/sprite function pointer | 8-byte in-band record; kinds PFLG / PFIM (image-relative offset) | imageStart + offset must name an executable function registered in the per-session persistent-function table (stableId↔native pair validated on both save and load; duplicates refused) |
| `DYNAMIC_BUFFER_PTR` | pointer into a registered mutable dynamic buffer (RAM scripts, mystery-event buffers, live G arena) | in-band 8-byte image record (the buffer lives in captured slices) + adapter revalidation on restore | resolved to the image pointer, then revalidated against the adapter's buffer registry: kind + ownerStorageId + generation + offset + instruction-boundary |
| `ROM_PROVENANCE_ADDR` | 4-byte GBA address retained as DATA (DMA3 destinations, routing operands, vaddress anchors, braille provenance) | data bytes; the single pointer-bearing protocol field (DMA3) is in-band zeroed with its identity in BATTLE_SIDECAR as an 8-byte data record | rehydrated to a native pointer, re-encoded to GbaAddr via `HostPointerToGbaAddr`; runtime handles refused |
| `SCALAR_OR_ID` | integer whose value may numerically resemble an address (apuCycle/apuFrame, heap header magic, IV/egg bits, link wire data, WAV table, species/move IDs) | verbatim bytes | verbatim; numerically stable under the fixed non-PIE base; cannot be mis-captured as resource references (modeled-scalar skip runs before the resource window) |
| `NULL_PTR` | inactive slot | zeroed bytes | stays NULL |
| `TRANSIENT_REFUSE` | illegal to capture while active | — | capture refuses with a precise diagnostic (save fails atomically) |

No `UNKNOWN_POINTER` class exists at completion: the walker's classification
ladder is total (adapter → range index → inactive scrub → NULL → function →
image → unmanaged/handle refusal → scalar), and every refusal names the
owning struct + field + classification.

## 2. Serialized slices (brief §2/§13)

| Section | Tag | Contents | Pointer walk |
|---|---|---|---|
| GAME_BSS | 1 | game bss range (includes static gHeap buffer and heap-allocated battle resources) | 4-byte stride, 8-byte windows |
| REGISTERS | 2 | REG_BASE 0x400 | no (runtimePointers=FALSE) |
| VIDEO_MEMORY | 3 | VRAM+PLTT+OAM | no |
| FLASH | 4 | FLASH_BASE (in-game save data) | no |
| EWRAM | 5 | gba_ewram section | yes |
| IWRAM | 6 | gba_iwram | yes |
| COMMON | 7 | gba_common | yes |
| GAME_DATA | 8 | 64-byte opt-in gba_data section (WAV[32] only) | yes (all scalar in practice) |
| FRAMEBUFFER | 9 | host framebuffer copy | no |
| TASK_SIDECAR | 10 | NUM_TASKS × 2 × 8B function records | record-typed |
| SPRITE_SIDECAR | 11 | (MAX_SPRITES+1) × 8B function records | record-typed |
| BATTLE_SIDECAR | 12 | 4 × 8B data records (DMA3 destinations) | record-typed |
| RTC | 13 | clock copy | no |
| RESOURCE_SIDECAR | 14 | 4 + 64×N resource records | record-typed |

## 3. G field-script surfaces (adapter: emerald_script_state.c; brief §6)

Static G execution surfaces (27 slots max) — `RESOURCE_PTR` (module key +
instruction-boundary-validated offset, schema 45/46, role CANONICAL):

| Surface | Slots | Slice | Boundary role | NULL/inactive |
|---|---|---|---|---|
| Context1.scriptPtr | 1 | GAME_BSS | INSTRUCTION_START | inactive ⇒ scrub |
| Context1.stack[i<depth] | ≤20 | GAME_BSS | NEXT_INSTRUCTION | inactive entries scrubbed |
| gRamScriptRetAddr | 1 | EWRAM | null or static-G NEXT_INSTRUCTION | NULL ok |
| gApproachingTrainers[0..1].trainerScriptPtr | 2 | COMMON | INSTRUCTION_START | NULL ok |
| sTrainerBattleEndScript | 1 | EWRAM | NEXT_INSTRUCTION | NULL ok |
| sTrainerA/BattleScriptRetAddr | 2 | EWRAM | ENTRYPOINT (generated export) | NULL ok |

Dynamic G surfaces — `DYNAMIC_BUFFER_PTR` (in-band image record +
adapter registry revalidation; kinds SAVE_RAM_SCRIPT / MYSTERY_EVENT_BUFFER /
STATIC_G_ARENA):

| Surface | Slots | Owner kind | Validation |
|---|---|---|---|
| Context1 IP/stack into RAM script | 21 | SAVE_RAM_SCRIPT | owner+generation+offset+instruction start |
| MysteryEvent context IP + stack | 21 | MYSTERY_EVENT_BUFFER | 17-op boundary bitmap |
| sMysteryEventScriptNativeBase | 1 | MYSTERY_EVENT_BUFFER | base, no instruction requirement |
| live G arena (vaddress base) | 1 | STATIC_G_ARENA | the G5 live arena buffer |

Virtual-address anchor `EmeraldScriptVirtualAnchor` (EWRAM): encodedVirtualBase
= `ROM_PROVENANCE_ADDR` (DATA) + bufferKind + ownerStorageId +
bufferGeneration + liveBaseOffset — no host pointer, no creator delta;
resolved by `ResolveVirtualTarget` against the current buffer registry
(stale generation refused).

TRANSIENT_REFUSE: Context2 (immediate context) active — mode≠0 or any
stack entry → capture refuses (ERR_CONTEXT2_ACTIVE).

Accompanying C text sidecars: 6 trainer text pointers → generic resource
window (text arena ranges). Field-execution resource-record max: 33/4,096
(G4 §8).

## 4. H battle-family surfaces (adapter: emerald_battle_state.c; brief §7)

36 H-typed slots + 9 engine-typed slots (H3 §4). `RESOURCE_PTR` (module key
+ boundary-validated offset, schemas 47-51, role CANONICAL):

| Surface | Slots | Slice | Family/schema | Boundary role |
|---|---|---|---|---|
| gBattlescriptCurrInstr | 1 | EWRAM | battle-script 47 | INSTRUCTION_START |
| gSelectionBattleScripts[4] | 4 | EWRAM | battle-script 47 | INSTRUCTION_START |
| gPalaceSelectionBattleScripts[4] | 4 | EWRAM | battle-script 47 | INSTRUCTION_START |
| battleScriptsStack.ptr[8] | 8 size-gated | GAME_BSS (gHeap) | battle-script 47 | NEXT_INSTRUCTION |
| gAIScriptPtr (shared) | 1 | EWRAM | battle-ai 49 ∥ contest-ai 50 (FAMILY_COUNT sentinel) | INSTRUCTION_START (quiescent stale; never executed) |
| AI_ScriptsStack.ptr[8] | 8 size-gated | GAME_BSS (gHeap) | battle-ai 49 | NEXT_INSTRUCTION (size 0 at capture by policy) |
| sBattleAnimScriptPtr | 1 | EWRAM | battle-anim 48 | INSTRUCTION_START |
| sBattleAnimScriptRetAddr | 1 | EWRAM | battle-anim 48 | NEXT_INSTRUCTION |
| ContestAIInfo.stack[8] | 8 size-gated | GAME_BSS (gContestResources heap) | contest-ai 50 | NEXT_INSTRUCTION |

Engine slots (always eligible; H bytecode on them refuses ERR_WRONG_CLASS):

| Surface | Slots | Slice | Class |
|---|---|---|---|
| battleCallbackStack.function[8] | 8 | GAME_BSS (gHeap) | ENGINE_IMAGE_PTR function |
| gAnimScriptCallback | 1 | EWRAM | ENGINE_IMAGE_PTR function |
| (+ gPreBattleCallback1, gBattleMainFunc, gBattlerControllerFuncs[4], BattleStruct.savedCallback — walker-modeled) | | | ENGINE_IMAGE_PTR function |

Maximum simultaneously active H resource records: 1 IP + 8 battle stack +
4 selection + 4 palace + 2 anim + 1 AI = **20** of 4,096 (H3 §5).
AI capture policy: quiescent stale IP relocated, never executed; AI stack
size 0 at every VBlank by construction. Contest stack exists only during a
contest (never simultaneously capturable with battle).

TRANSIENT_REFUSE: field-effect bytecode has no persistent IP (interpreter
cursor is function-local); an FE-arena pointer in any serialized field
refuses ERR_TRANSIENT. Inactive stack scratch holding a live H pointer
refuses ERR_UNKNOWN_POINTER (scrub only when zero).

## 5. Generic resource-window surfaces (CaptureResourceWindow)

Any 8-byte window in a walked slice that resolves to a registered range
becomes a `RESOURCE_PTR` record (trainer/pokemon gfx tables, object-event,
tileset, layout, audio arena pointers, R13-C text pointers — TextPrinter
currentChar, gFonts/sStringPointers, trainer speech buffers —, R13-F map
header/event/connection pointers, frontier facility pointers). In-hull
without a range = hard capture failure (never silent). Frontier AUX
refusals unregister the two exact arena keys (R13-G4 regression).

## 6. Engine-image surfaces (walker-modeled; brief §8/§9)

Function-pointer fields (→ PFIM/PFLG records, persistent-function table
validation):
- `gTasks[].func` (NUM_TASKS), `gMain` callbacks ×7, `gSprites[].callback`,
  `SpriteTemplate.callback` (sidecars + battle templates), `TextPrinter[].callback`
  (32 windows), battle callback stack + gPreBattleCallback1/gBattleMainFunc/
  gAnimScriptCallback/gBattlerControllerFuncs[4]/BattleStruct.savedCallback
- TASK_SIDECAR: sTaskFollowupFuncs + sTaskStoredFunctions (NUM_TASKS × 2)
- SPRITE_SIDECAR: sSpriteStoredCallbacks (MAX_SPRITES+1)

Data-pointer fields (→ PDLG/PDIM records):
- `SpriteTemplate.oam/anims/images/affineAnims`, `SpriteFrameImage.data`,
  `gSprites.anims/images/affineAnims/template/subspriteTables`,
  `TextPrinter.printerTemplate.currentChar`, `gWindows[].tileData`,
  battle roots (gBattleAnimBgTileBuffer, gBattleStruct, gBattleResources,
  gBattleSpritesDataPtr, gMonSpritesGfxPtr, gBattleMsgDataPtr,
  gAnimDisableStructPtr, healthbox data, battle buffer roots), heap-interior
  pointers (battle stacks when families were compiled — now H-owned when
  live), and every other game-image pointer the walker finds.

The image-relative model is semantic (offset), not raw-address: the
persistent-function table additionally validates that each stableId maps to
the same executable function on both save and load; the non-PIE fixed base
makes offsets numerically stable, but validation never depends on it
accidentally.

## 7. Refusal surfaces (brief §29 census)

| Refusal | Trigger | Phase |
|---|---|---|
| Context2 active | mode/scriptPtr/stackDepth/stack nonzero | capture preflight |
| FE transient pointer | FE-arena pointer in any serialized field | capture |
| H bytecode on engine slot | callback surface holding arena pointer | capture |
| H pointer on non-H field | arena pointer on unowned surface | capture |
| inactive scratch with live pointer | inactive stack entry nonzero into arena | capture |
| stale family generation | stamp ≠ current generation id | capture |
| wrong boundary | middle-of-operand / hole / padding / past-end / data span / non-export entrypoint | capture + resolve |
| wrong family / schema / type / role | identity mismatch | capture + resolve |
| alias identity | zero-width alias key | resolve |
| dynamic buffer unknown/stale/OOB | registry miss, generation mismatch, bounds | capture + resolve |
| unmanaged host pointer | high mapped address outside the image | capture |
| host_data pointer | value inside host_data span | capture |
| E/F runtime handle | registered process-local handle | capture final + load verify |
| hull without identity | resource-owned, no registered range | capture |
| sidecar overflow | > 4,096 records | capture |
| v4 state / wrong version | formatVersion ≠ 5 | load (pre-parse) |
| buildId mismatch | header buildId ≠ active | load |
| fingerprint mismatch | session content digest differs | load |
| malformed container / CRC | header/section/payload CRCs | load |
| sidecar structural corruption | tag/size/count/offset/key/role/reserved/ordering/overlap | load (pre-mutation) |
| unresolved record | unknown key / identity mismatch / OOB offset / missing range / no session | load (pre-mutation) |
| republish pre-flight failure | trainer/audio republish not viable | load (pre-mutation) |
| rehydrated runtime handle | battle sidecar resolves to a handle | load |
| battle sidecar mismatch | sidecar record without matching DMA3 command | load |

Every refusal is explicit, test-covered, and leaves prior state untouched
(all-or-nothing commit; canary proofs).

## 8. Heap-interior pointer policy (brief §10/§11)

gHeap is a top-level `src/*.o` static buffer (`src/main.c:67` PORTABLE
branch) — it lands in the GAME_BSS slice (verified: gHeap 0x18f9dc0 inside
game_bss 0x18d51e0..0x191ba40); every heap-interior pointer is therefore an
image-relative `ENGINE_IMAGE_PTR` record (PDIM). Host malloc pointers (arena
allocations) never enter slices (arenas live outside every slice; pointers
INTO arenas are `RESOURCE_PTR` records). Pointers to host-only allocations
that appear in a slice are `TRANSIENT_REFUSE` (unmanaged native pointer) at
capture — the hard gate: no persisted unrelocated heap pointer can exist.

Dynamic-buffer registry (complete; agent-B verified): SAVE_RAM_SCRIPT
(script.c:440-456, saveblock ramScript storage), MYSTERY_EVENT_BUFFER
(mystery_event_script.c:80-105, heap recvBuffer + per-card 17-op bitmap),
STATIC_G_ARENA (emerald_script_compat.c:2300-2310, the live G arena);
CAPTURED_BUFFER is defined with no registration site (dead enum value,
noted in the census). Overlap rejected; same (kind, owner) re-registration
is a generation replacement.

## 8a. R13-I finding F1 (HARD): frontier host_data pointer retention — FIXED

`gFacilityTrainers` / `gFacilityTrainerMons` (EWRAM, battle_tower.c:47-48)
hold raw pointers into the published HOST_DATA fill targets
(gBattleFrontierTrainers/Mons + the 3 tent trainer/mon table pairs; writers:
frontier_util.c:933, battle_tower.c:3249-3359, battle_pike.c:1444-1457,
battle_factory.c:331/406). The walker's classification ladder silently
serialized those raw host addresses (not modeled, no registered range,
host_data excluded from the image gate, below the high-host band, not a
known-data field → scalar `continue`). Same-binary round-trip masked it.

R13-I closure: the eight HOST_DATA fill targets are now registered as
COMPAT_OBJECT spans (the emerald_map_compat.c map-headers precedent) —
capture emits key+offset `RESOURCE_PTR` records, load resolves through the
index. Registration is idempotent (RemoveFrontierRanges key-removal
preamble), clear removes all ten frontier keys. Live range count:
**6,382 → 6,390 / 8,192** (justified §35 deviation: the pin predates this
finding; the alternative is refusing saves inside frontier facilities, a
gameplay regression; no existing sidecar could carry these identities
without ranges). No format change, no cap change.

## 8b. R13-I finding F2: FLASH-slice handle-gate asymmetry — FIXED

The FLASH slice was the one raw-copy section never audited for E/F runtime
handles (the walker's save-final audit doesn't run there; load validation
skipped non-runtimePointers sections). The only producer of a handle byte
into save-block data is `ObjectEventTemplate_SetScript`'s non-compat
fallback (global.fieldmap.h:138 — unreachable today: every object-event
script is compat-resolvable). R13-I adds the same registered-handle gate on
the FLASH slice at capture (`save-flash-final`) and load
(`load-flash-verify`) — the invariant is now structural, not disciplinary.

## 9. Pointer-width / struct-copy audit (brief §12/§13)

- `struct HostPersistentAddress` is 8 bytes (STATIC_ASSERT), kinds PDLG/
  PDIM/PFLG/PFIM; all serialized records are bytewise LE stores (StoreLe32/
  memcpy of fixed-width fields), never host-struct dumps.
- 4-byte GBA fields: the DMA3 destination is the only pointer-bearing
  4-byte protocol field; in-band zeroed + BATTLE_SIDECAR identity; no host
  pointer is ever stored in a 4-byte serialized field (agent-C sweep: zero
  sites; the u16-pair callback packings — shop.c, battle_factory_screen.c,
  pokeball.c, battle_anim_mons.c, easy_chat.c, union_room.c, list_menu.c —
  all round-trip GBA-space provenance or in-image identities; the two
  raw-cast sites party_menu.c:1897 / list_menu.c:1068 are unreachable).
- Task/sprite sidecar records are written bytewise (alignment-safe over the
  container buffer); walked-slice records are rewritten in place at
  pointer-aligned offsets only (STATIC_ASSERT layout pins).
- No serialization path memcpy's a host struct containing pointers into a
  slice; the walker reads 8-byte windows and writes back 8-byte records.
- Save blocks are GBA-shaped (no host-width members; SaveBlock1's only
  pointer-shaped field is ObjectEventTemplate.script = u32 GbaAddr on
  LINUX64, normalized at the accessor boundary). The in-game .sav byte
  copies (save.c:178-210) are safe by construction; the FLASH slice now
  carries the handle gate (finding F2) making the invariant structural.
- The gHeap header-magic and apuCycle/apuFrame false-hit windows are
  documented scalar-coincidence classes, explicitly skipped.

## 10. Range registry census (brief §14 — measured)

Live index: **6,390 / 8,192** ranges (6,382 pre-R13-I + 8 frontier
fill-target spans, finding F1), hulls 7. Measured buckets from the loader
test census (78,512 checks, pre-F1 run at 6,379):

| type | schema | role | count | family |
|---|---|---|---|---|
| 16 | 7 | 2 | 1 | (gameplay levelup arena) |
| 16 | 41 | 2 | 1 | map-header |
| 8 | 1 | 2 | 10 | (fonts) |
| 16 | 43 | 2 | 1 | map-events |
| 16 | 45 | 0 | 515 | script-field (G) |
| 16 | 46 | 0 | 8 | script-routing (G) |
| 7 | 1 | 0 | 882 | (tilemap = layout family) |
| 2 | 1 | 1 | 999 | (tile-graphics legacy-LZ = object-event/trainer/pokemon) |
| 3 | 1 | 1 | 872 | (palette legacy-LZ) |
| 4 | 1 | 0 | 253 | (sprite-sheet) |
| 3 | 1 | 0 | 1239 | (palette canonical) |
| 15 | 2 | 0 | 5 | (instrument-bank) |
| 10 | 1 | 0 | 569 | (audio-sample) |
| 11 | 1 | 0 | 530 | (music-sequence) |
| 15 | 1 | 2 | 197 | (voicegroup compat) |
| 6 | 1 | 0 | 70 | (tileset) |
| 6 | 2 | 0 | 70 | (tileset secondary) |
| 2 | 1 | 0 | 133 | (tile-graphics canonical) |
| 16 | 44 | 2 | 1 | map-connections |
| 16 | 4 | 2 | 1 | (text system arena) |
| 16 | 17 | 2 | 1 | trainer-party |
| 16 | 20 | 2 | 1 | encounter-slot |
| 9 | 1 | 2 | 16 | (text blobs) |
| 16 | 22 | 2 | 1+ | frontier-mon-set |
| 16 | 37 | 2 | 1+ | frontier-wild |

(+ the 5 H arena ranges in the production loader = schemas 47-51, one
each.) Ranges are key+type+schema+role+span; sorted by base,
non-overlapping (registration rejects overlap), nonzero-length;
identity-based unregister (position-independent); ResolveByKey is the only
load-direction materializer.

## 11. Sidecar capacity (brief §16)

- G static field-execution max: 27 + 6 trainer-text = 33
- H max simultaneously active: 20 (+8 AI-stack +8 contest-stack only in
  debug captures that production cannot produce)
- R10 projection: typical field save 6–14 records, mid-battle 10–35, worst
  plausible ~60; audio-era image entries add the generic arena records
- Cap 4,096 — measured worst case [PENDING battery measurement]

## 12. Compatibility fingerprints (brief §27)

Header: buildId (executable identity) + contentFingerprint (session
content digest: gameId + adapter version + RESOURCE_API_VERSION +
logicalContentDigest + provider list). Load requires both; no force-load.
A state from a different content set refuses with expected-vs-active
fingerprint diagnostics. Deterministic: content-derived only.

## 13. Deterministic serialization (brief §32)

Sidecar records carry no host addresses; in-band bytes are zeroed; the only
host-dependent values are excluded (arena pointers never in-band). Proven by
the `determinism` mode: the same semantic state saved at two forced-different
arena bases produces byte-identical payload sections, with exactly two
allowed-differ fields — the header frame counter and the script
generationStamp (session-lifecycle bookkeeping in the GAME_DATA slice; the
raw stamps provably differ across the two saves, proving the restage moved
the generation while everything semantic stayed identical).
