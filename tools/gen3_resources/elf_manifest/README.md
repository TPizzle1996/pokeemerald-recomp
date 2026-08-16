# gen3-elf-manifest — Stage R1A extraction-metadata generator

Deterministic maintainer-side tool that produces the extraction manifest for the
Gen3 resource mod. It takes the **semantic** bindings
(`resources/extraction/emerald/bpee01/bindings.toml`), the **catalog contract**
(`resources/catalogs/emerald/catalog.toml`), the linked GBA **ELF** and the
retail **ROM**, validates every resource end-to-end, and emits a
byte-for-byte deterministic TOML manifest (records sorted bytewise by canonical
id). No ROM offsets are hand-maintained anywhere — the offset for each resource
is derived from the ELF symbol table (`st_value - 0x08000000`) and cross-checked
against the ROM slice.

## Building

```sh
make -C tools/gen3_resources/elf_manifest
```

Produces `gen3-elf-manifest` and `fixture_build`. Build isolation is
`-std=c99 -Wall -Wextra -Werror` plus the two include roots for the shared Gen3
core (`sha256.c`, `resource_id.c`). The tool links the exact M0/M1 key-derivation
code, so manifest keys are recomputed with the same algorithm that runtime
resource IDs use.

## Usage

```sh
# From the repository root (binding-relative artifact paths resolve against cwd):
tools/gen3_resources/elf_manifest/gen3-elf-manifest \
  --catalog resources/catalogs/emerald/catalog.toml \
  --bindings resources/extraction/emerald/bpee01/bindings.toml \
  --elf <pokeemerald.elf> \
  --rom  <pokeemerald.gba> \
  --output <manifest.generated.toml>
```

Options: `--check` (fail if `--output` differs byte-for-byte from deterministic
regeneration), `--rom-sha1`, `--rom-game-code`, `--rom-maker-code`,
`--rom-revision`, `--provenance` (deterministic one-line provenance comment).
Defaults are the retail Emerald identity (BPEE / "01" / revision 0 /
SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).

## Validation pipeline (per resource)

1. ROM is exactly 16 MiB; game code `BPEE`, maker `01`, revision 0; SHA-1 matches.
   ROM SHA-256 is recorded in the manifest.
2. ELF parses (ELF32, little-endian, EM_ARM); the binding symbol must be an
   allocated ROM object with `st_value >= 0x08000000` and non-zero `st_size`.
3. ROM offset = `st_value - 0x08000000`; the `[offset, offset + st_size)` range
   must fit inside the ROM.
4. Three-way equality: source artifact bytes == ELF symbol bytes == ROM slice
   bytes.
5. Strict GBA LZ77 decode of the encoded bytes (rejects truncation, invalid
   backreferences, overflow, over-read, and decoded-size mismatch). Decoded size
   must equal `expected_decoded_size` and the bytes must equal the canonical
   decoded artifact (`source_artifact` with `.lz` removed).
6. Canonical key recomputed with the existing M0/M1 algorithm
   (`SHA-256("gen3-resource-id-v1\0" + canonical name)`); SHA-256s of the encoded
   source and the canonical decoded bytes are recorded.

## The checked-in manifest is fixture-derived

There is no retail ROM or GBA ELF in this environment, so
`resources/extraction/emerald/bpee01/manifest.generated.toml` is generated from a
**deterministic synthetic fixture**: a 16 MiB ROM carrying the real retail header
identity (BPEE / "01" / revision 0 / valid complement) that embeds the real
Brendan source artifacts at fixed offsets (front sheet at `0x300000`, palette at
`0x310000`), plus a matching ELF whose symbol table names them
`gTrainerFrontPic_Brendan` / `gTrainerPalette_Brendan`. The fixture ROM's SHA-1 is
`092ee293c6e4381074682375249561bbc41bec44` (recorded in the manifest), which is
*not* the retail SHA-1 — the `# provenance:` comment and the `rom_sha1` line flag
the origin.

```sh
# Reproduce the fixture and regenerate the manifest:
tools/gen3_resources/elf_manifest/fixture_build \
  --elf /tmp/fixture.elf --rom /tmp/fixture.gba          # prints rom_sha1
tools/gen3_resources/elf_manifest/gen3-elf-manifest \
  --catalog resources/catalogs/emerald/catalog.toml \
  --bindings resources/extraction/emerald/bpee01/bindings.toml \
  --elf /tmp/fixture.elf --rom /tmp/fixture.gba \
  --rom-sha1 <rom_sha1 from above> \
  --provenance "generated from the synthetic bpee01 fixture (...)" \
  --check --output resources/extraction/emerald/bpee01/manifest.generated.toml
```

When a matching GBA toolchain and a locally built `pokeemerald.elf` /
`pokeemerald.gba` become available, run the same command without
`--rom-sha1` (defaults to the retail hash) to derive the real ROM offsets and
hashes. Exact-GBA verification against the retail ROM is currently
**UNAVAILABLE** in this environment.
