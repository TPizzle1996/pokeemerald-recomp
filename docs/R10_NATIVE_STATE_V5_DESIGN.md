# R10 — Native State v5 Design

Contract for the v5 implementation. Grounded in the R10-A audit
(`docs/R10_NATIVE_STATE_V4_AUDIT.md`); its decisive finding: **no
resource-owned pointer enters a serialized slice today** (the migrated tables
live in host `.data`; the GAME_DATA slice is a 64-byte opt-in section). v5
makes the *first* such pointer safe and proves the mechanism with the real
production pack and real migrated battle resources.

## B. State v5 format

- `NATIVE_STATE_FORMAT_VERSION` = 5. Header struct unchanged; the
  `contentFingerprint[64]` field becomes the hex of the computed session
  fingerprint (§F).
- New section tag `STATE_SECTION_RESOURCE_SIDECAR = 14` (sectionCount ≤ 16
  accommodates it). v5 states always carry the sidecar section (record count
  0 when nothing was captured). v4 files contain no tag-14 section and are
  rejected by version before any section parsing (§I).
- All sidecar integers little-endian, records fixed-size and packed, no host
  padding, no pointer values, no handles.

### Sidecar section payload

```
u32 LE recordCount
record[recordCount]          // each exactly 64 bytes
```

### Resource-reference record (64 bytes)

| field | type | meaning |
|---|---|---|
| sectionTag | u32 | owning serialized section tag (walked slices only) |
| fieldOffset | u32 | byte offset of the 8-byte pointer field within that section's payload |
| resourceKey | u8[32] | `Gen3ResourceKey` (32 raw bytes) |
| resourceType | u32 | `Gen3ResourceType` |
| resourceSchema | u32 | schema/version |
| representationRole | u32 | 0 = canonical payload, 1 = legacy/literal-LZ payload, 2 = compatibility object/table |
| rangeOffset | u32 | offset of the referenced byte within the immutable resource range |
| reserved | u32 | must be 0 |
| reserved2 | u32 | must be 0 (makes the packed record exactly 64 bytes with no compiler padding) |

### Parse validation (any failure rejects the whole state)

- `(sidecarSize - 4) % 64 == 0` and `recordCount == (sidecarSize - 4) / 64`.
- `recordCount ≤ 4096`.
- `sectionTag ∈ {GAME_BSS, EWRAM, IWRAM, COMMON, GAME_DATA}`.
- `fieldOffset % 4 == 0`, `fieldOffset + 8 ≤ sectionSize` of that section in
  the same file.
- Records ordered strictly increasing by `(sectionTag, fieldOffset)`; within
  a section consecutive fields must not overlap (`Δ fieldOffset ≥ 8`) — no
  duplicate or conflicting field records.
- `representationRole ≤ 2`, `reserved == 0`, `resourceKey` nonzero.
- Resolution (against the active session's range index, §C): the key must
  name a registered range whose type, schema, and role equal the record's,
  and `rangeOffset < range.length`. Unknown role/type/schema/offset → reject
  with the §J diagnostic.

## C. Reverse resource-range index

New module `src/emerald/resources/emerald_resource_ranges.c` (+ header),
owned by the active resource session (the session seam holds the instance;
the compat seams register their images after build; `native_state.c` reads
it through `EmeraldResourceSession_GetRangeIndex()`).

- One registered range per compat-image entry: the entry's **stream region**
  `[bytes + streamArenaOffset + entry.streamOffset, + entry.streamSize)`,
  carrying the entry's `Gen3ResourceKey`, type, schema, and
  `ROLE_LEGACY_LZ (1)`. Key/type/schema come from the `Gen3ResourceView` the
  seam already resolved at image build (view.key/type/schema). Pokémon image
  (1804 ranges) + trainer image (ranges per its entries) ≈ 2040 ranges.
- Two arena hulls (trainer image `bytes[]` span, pokémon image span) so a
  pointer into resource-owned memory that matches **no** range is detectable
  as a capture error, never silently omitted.
- Registration: ranges sorted by base; **overlapping ranges rejected at
  registration** (no ambiguous overlaps unless modeled — none are). All
  address arithmetic via `uintptr_t` with explicit overflow-safe ordering.
- Lookup: binary search → exactly one range (offset = ptr − base), none, or
  ambiguity (impossible post-validation, still checked).
- Lifecycle: index lives and dies with the session; the session outlives all
  gameplay; lookups only happen during capture/load while the session is
  active. No runtime-handle identity anywhere.
- Ranges are **identity + offset only** — the index never stores a pointer in
  serialized form, and resolution output is a pointer valid in the current
  process.

Unit tests: range start, interior, last valid byte, one byte before/after,
overlapping registration rejected, base+length overflow rejected, ambiguous
lookup.

## D. Save capture

In `NormalizeRuntimeBytes`, at each 4-byte stride, **before** the existing
classification (and reading the 8-byte window from `source`, which capture
never mutates):

1. If `offset + 8 ≤ size`, read the 8-byte window and look it up in the
   range index (when a session exists):
   - exact range match → append a record `{sectionTag, offset, key, type,
     schema, role, ptr − range.base}` and **zero the 8 bytes in `dest`**;
     continue. (Zeros are inert to every existing check, including
     `ValidateNoRuntimeHandles`.)
   - inside an arena hull with no range match → fail ("resource-owned pointer
     lacks a registered identity").
   - ambiguous → fail.
2. Otherwise the existing walker logic is unchanged (its "unmanaged native
   pointer" gate now only ever sees genuinely non-resource host pointers).
3. Records accumulate in a capture-side array; exceeding the 4096 limit is a
   hard failure (never silently omit).
4. The sidecar section (tag 14) is appended after the last slice section.
   The file allocation reserves the maximum sidecar size up front
   (4 + 64·4096); header totals/sectionCount reflect the actual record
   count at write.

Capture fails (never silently persists) on: ambiguity, no identity,
unregisterable resource-owned pointer, sidecar overflow. A state with no
session simply produces no records (fingerprint = legacy constant; §F).

## E. Transactional load

`LoadStateFromPath`, in exact order:

1. Read header; reject non-v5 with precise version diagnostics (§I), then
   structural bounds (totalSize, headerSize, sectionCount ≤ 16, payloadSize,
   payloadCrc).
2. Executable/build-ID check (unchanged).
3. Session fingerprint (§F): require an active session for v5; compute and
   compare against `header.contentFingerprint`; mismatch → reject with
   expected-vs-active report. No force-load path.
4. Read **all** section payloads into one staging buffer, validating each
   section CRC as it is read. Live memory is untouched.
5. Locate exactly one tag-14 sidecar; parse and validate every record (§B).
6. Resolve **every** record against the active range index; any failure
   (unknown key, type/schema/role mismatch, rangeOffset OOB, missing range)
   → reject with the §J diagnostic. **Nothing has been mutated.**
7. Commit: restore sections in slice order (existing `RestoreSlice`
   semantics; resource fields are zeros in-band and decode harmlessly).
8. Patch pass: write each resolved 8-byte pointer into the restored section
   memory at `source + fieldOffset`.
9. Existing post-load lifecycle (RTC/framebuffer/audio/clock restore,
   `EmeraldResourceCompat_Republish`) unchanged.

No partial restoration: any step-1–6 failure leaves game memory byte-exact.

## F. Session content fingerprint

Computed at session build by the session seam and stored; exposed via
`EmeraldResourceSession_GetSessionContentFingerprint()`. Raw 32-byte SHA-256
(`Gen3Sha256`) over the exact construction:

```
SHA-256(
    "gen3-session-content-v1" || 0x00
    || LE32(len(gameId)) || gameId
    || LE32(EMERALD_RESOURCE_SESSION_ADAPTER_VERSION)      // = 1
    || LE32(RESOURCE_API_VERSION)                          // (MAJOR<<16)|(MINOR<<8)|PATCH
    || baseProvider.logicalContentDigest[32]               // zeros if absent
    || LE32(providerCount)
    || for each provider, ascending precedence:
         LE32(kind)
         LE32(precedence)
         LE32(len(providerId)) || providerId
         LE32(len(providerVersion)) || providerVersion
         logicalContentDigest[32]                          // zeros if absent
)
```

Hashes logical content only — no paths, timestamps, pointers, or PIDs;
deterministic across machines; provider order is meaningful. Today:
providerCount = 1, the ROM_BASE provider. Executable identity stays the
separate buildId check. Header field carries the hex (64 chars).

Equivalent content at a different path → identical fingerprint (the provider
digest is content-derived; TEST 5 proves it). Changed provider content →
different fingerprint (TEST 4).

Sessions absent (non-Linux targets): fingerprint = the legacy constant
string, preserving the old behavior exactly.

## I. State v4 policy

**Reject v4.** A v4 file cannot express resource-backed pointers; accepting
it would either reintroduce the unsafe class or require maintaining the old
walker semantics forever. Rejection message names the format explicitly:
"state format v4 predates resource-aware serialization and cannot safely
express resource-backed pointers; re-save with a v5 build". Tests: (i) a
valid v4 file → precise v4 rejection; (ii) a v4 file with a forged tag-14
section appended → still rejected by version, sidecar never parsed; (iii) an
unsupported version (0, 6, 0xFFFFFFFF) → clear "unsupported state version";
(iv) a v5 file cannot be opened as v4 (version gate is the first check).

## G/H. Test design

Cross-process harness `tests/emerald_resource_state_test.c` + runner
`run_emerald_resource_state.sh`, compiling the **real** `native_state.c` +
`host_memory.c` + the real seams + real pack chain + the real `src/data.c`
native branch (the real_tables recipe) + a small platform-stub TU (the 18
`Platform_*` + 2 state-size stubs; storage stubs do real file I/O in a temp
dir; runtime-range stubs model the harness's own image). The harness runs
twice (creator process / loader process) under the runner.

- The captured GAME_DATA slice source = the **real published battle table
  memory** (real `gMonFrontPicTable` rows holding real arena pointers into
  the production-pack-derived session image) plus a real game-state struct —
  the real battle resource entering runtime game state, with real keys, real
  types/schemas, real offsets.
- TEST 1: same-process save → load → pointers patched to the same arena,
  game data intact.
- TEST 2/3: creator exits; loader (fresh process, ASLR + fresh mallocs →
  provably different arena bases) loads; row `.data` equals the loader's own
  arena pointer (identity by key, never by address equality).
- TEST 4: loader builds its session from a temp **copy** of the pack with one
  payload altered → fingerprint mismatch → transactional rejection, canary
  proves live memory untouched.
- TEST 5: identical pack content at a different path → fingerprint equal →
  load succeeds.
- TEST 6: fingerprint unit test — 2-provider construction vs reversed
  precedence → digests differ.
- TEST 7: capture normally, then corrupt one record's key in the file
  (recomputing the section CRC) → resolution failure → reject before
  mutation (canary).
- TEST 8: corrupt sidecar matrix — bad section tag, field offset OOB,
  all-zero/bad key, bad role, bad schema, resource offset OOB, truncated
  sidecar, oversized count, duplicate/overlapping field records — every case
  rejects safely (parser diagnostics; no crash; canary intact).
- H parity: after load, each patched stream byte-compares against the
  pack-derived canonical payload re-encoded (the real_tables parity chain
  already proves decode parity ⇒ render identity).

R9 isolation is untouched: no payload is linked back into the executable;
the pack is opened read-only; every mutation happens on temp copies.
