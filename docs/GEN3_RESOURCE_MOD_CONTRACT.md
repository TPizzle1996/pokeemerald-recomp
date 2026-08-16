# Shared Gen3 resource and mod contract

This document freezes the Stage M0/M1 resource contract. Stage M1 implements
only the platform-neutral, in-memory catalog/provider/resolver core. It does
not load files, parse mod manifests, extract ROM data, or alter Emerald.

## Identity

A resource's permanent public identity is its canonical name. Accepted names
are exact byte strings; they are never lowercased, trimmed, Unicode-normalized,
alias-resolved, or otherwise rewritten before validation or hashing.

Names are at most 255 bytes, contain exactly one namespace colon, use `/` as
the path separator, and contain only lowercase ASCII letters, digits, `.`, `_`,
and `-` within non-empty segments. `.` and `..` path segments, backslashes,
percent escapes, whitespace, uppercase, and empty segments are invalid.

Reserved namespaces are `gen3:`, `emerald:`, `firered:`, `engine:`, and `mod:`.
Base-catalog maintainers own the first four. A mod with ID `org.example.foo`
may define new resources only below `mod:org.example.foo/`. Cross-mod
references will require a declared dependency when mod loading is implemented.

The stable binary identity is the 32-byte standard SHA-256 digest of these
exact bytes:

```text
"gen3-resource-id-v1\0" + canonical-name-bytes
```

The prefix's terminating NUL is included. A key is serialized as 32 bytes, not
as host-endian integers or a truncated hash. Hex is diagnostic text only.
Providers retain canonical names. Registration recomputes keys, and two
different names with the same key are rejected. Tests use an internal injected
key derivation seam to exercise that rejection without weakening production
SHA-256.

`Gen3ResourceHandle` is a session-local `uint32_t` index assigned after sorting
canonical names bytewise. Handles are efficient runtime indices but are never
serialized or used as persistent identity.

## Catalog contracts and resource types

The catalog owns each canonical resource's type, schema version, and whether a
valid base-game candidate must supply it. Providers supply implementations;
they cannot redefine a catalog contract.

Stage M0 reserves these resource-mod types: bitmap, tile graphics, palette,
sprite sheet, sprite metadata, tileset, tilemap, font, text, audio sample,
music sequence, sound effect, cry, and bounded arbitrary binary. Structured
map, script, encounter, item, move, species, and trainer data require later
domain-specific contracts and are not generic M1 resources.

Type mismatch and schema mismatch are distinct failures. A malformed provider
entry can be required for that provider, which makes candidate construction
fail. This is separate from `requiredForBase`, which says a valid base game
must have a valid implementation from a base-kind provider.

## Providers and resolution

Provider metadata contains an explicit, unique precedence integer. Precedence
is never inferred from filesystem enumeration, registration timing, pointer
address, or container iteration. Providers are sorted from low to high
precedence and resource resolution is last-valid-provider-wins.

For an optional invalid higher-precedence entry, resolution records a
structured rejection and continues downward until a valid implementation is
found. A required invalid provider entry rejects the entire candidate. A
missing catalog resource marked required for the base game is reported
separately, even if a non-base provider supplies that name.

Provider insertion order does not affect canonical identity, handle assignment,
or results unless explicit precedence differs. Traces walk providers in
descending precedence and contain no timestamps, pointer values, or unstable
container order.

## Transactional publication and ownership

Catalogs copy canonical names. Providers copy IDs, versions, canonical names,
and payload bytes. Candidates copy their catalog and providers. Successful
snapshots own their complete immutable copies of contracts, providers,
payloads, and resolved winner records. Resource views borrow from a published
snapshot and remain valid until that snapshot is destroyed.

Diagnostic and trace lists own their records until their explicit destroy
functions are called. Human-readable descriptions are derived from stable
reason enums.

Candidate construction occurs off to the side. Publishing atomically swaps a
complete validated snapshot into a registry. A rejected candidate produces no
snapshot and cannot modify the active one.

## Version constants

```text
RESOURCE_API_VERSION = 1.0.0
MOD_MANIFEST_VERSION = 1
MOD_PACK_VERSION = 1
RESOURCE_PACK_FORMAT_VERSION = 1
```

Identity grammar, key derivation, existing type/schema meanings, or precedence
semantics require a resource API major version change. New optional types,
fields, and canonical IDs are minor additions. Physical pack or manifest
incompatibilities increment their integer versions.

## Security and Stage M1 boundary

Resource mods are data-only. Executable/native content is not supported by
this contract. Stage M1 uses synthetic in-memory providers only and has no
filesystem, archive, ROM, SDL, Emerald, save-state, compatibility-table, or
hot-reload integration.
