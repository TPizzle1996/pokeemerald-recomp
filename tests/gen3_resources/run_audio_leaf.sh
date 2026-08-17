#!/usr/bin/env bash
#
# R12-B audio leaf ownership migration test runner (tests A-G).
#
#   A-G  tests/emerald_audio_compat_test.c against the REAL production pack:
#        A counts (105/51/388/25 = 569 leaves, pack 5087 entries),
#        B exact extraction (arena bytes == pack payloads == ROM slice,
#          every leaf inside the verbatim zone, no overlaps),
#        C WaveData2 structural validation per sample,
#        D waves exactly 16 bytes,
#        F compat publication (session -> TryInitialize -> arena, leaf
#          spans/bytes, relocated session/arena, republish, clear/shutdown,
#          invalid arguments),
#        G failure matrix (missing leaf, unresolvable leaf, wrong type,
#          wrong schema, wrong size, corrupt payload, out-of-span
#          placement, pack duplicate-key guard).
#   E    determinism/provenance re-proofs:
#        - gen3-pack-build --check reproduces the committed production pack
#          byte-for-byte from the 7 manifests + 7 catalogs;
#        - gen3-elf-manifest --check re-derives the audio manifest from the
#          qualified ELF + retail-matching ROM (the tool re-proves the
#          three-way ELF == ROM == manifest canonical slices).
#
# Build-isolated like the other Gen3 runners: gen3 core + session + seam
# only, no Emerald defines, no SDL, never launches the game. The seam is
# platform-neutral, so no native tables are linked.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"
audio="$root/resources/extraction/emerald/bpee01/audio"
pack="$root/games/emerald/base/emerald-bpee01-v1.rpack"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

failures=0
passes=0
pass() { passes=$((passes+1)); echo "  PASS $1"; }
fail() { failures=$((failures+1)); echo "  FAIL $1" >&2; }

echo "== E1: production pack determinism (gen3-pack-build --check) =="
make -C "$root/tools/gen3_resources/pack_build" all >/dev/null
if "$root/tools/gen3_resources/pack_build/gen3-pack-build" \
    --rom ../pokeemerald-reference/pokeemerald.gba \
    --output "$pack" \
    --manifest "$root/resources/extraction/emerald/bpee01/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/object_event/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/tileset/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/layout/manifest.production.toml" \
    --manifest "$audio/manifest.production.toml" \
    --catalog "$root/resources/catalogs/emerald/catalog.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/object_event/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/tileset/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/layout/catalog.generated.toml" \
    --catalog "$audio/catalog.generated.toml" \
    --check > "$tmp/pack_check.log" 2>&1; then
    pass "pack reproduces byte-for-byte (5087 entries)"
else
    fail "pack --check: $(tail -3 "$tmp/pack_check.log" | tr '\n' ' ')"
fi

echo "== E2: audio manifest provenance (gen3-elf-manifest --check) =="
make -C "$root/tools/gen3_resources/elf_manifest" all >/dev/null
if "$root/tools/gen3_resources/elf_manifest/gen3-elf-manifest" \
    --catalog "$audio/catalog.generated.toml" \
    --bindings "$audio/bindings.generated.toml" \
    --elf ../pokeemerald-reference/pokeemerald.elf \
    --rom ../pokeemerald-reference/pokeemerald.gba \
    --rom-sha1 f3ae088181bf583e55daf962a92bb46f4f1d07b7 \
    --qualification production \
    --provenance "generated from the qualified pret reference build (d8e405c4f6b48f1faf3b26a3e045f0df2ff3ecb7 + upstream symbol rename b89722500) ELF + retail-matching recomp ROM; BPEE01 Rev 0 (SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7)" \
    --output "$audio/manifest.production.toml" \
    --check > "$tmp/manifest_check.log" 2>&1; then
    pass "audio manifest re-derives byte-for-byte (three-way ELF == ROM == manifest)"
else
    fail "audio manifest --check: $(tail -3 "$tmp/manifest_check.log" | tr '\n' ' ')"
fi

echo "== A-G: audio leaf seam tests (emerald_audio_compat_test.c) =="
gcc -std=gnu99 -O2 -Wall -Wextra \
    -iquote include -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/sha1.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_writer.c" \
    "$core_dir/resource_pack_provider.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$emerald_dir/emerald_rom_profile.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$emerald_dir/emerald_resource_ranges.c" \
    "$emerald_dir/emerald_resource_session.c" \
    "$emerald_dir/emerald_audio_compat.c" \
    "$root/tests/emerald_audio_compat_test.c" \
    -o "$tmp/emerald_audio_compat_test"
if "$tmp/emerald_audio_compat_test" "$pack" "$tmp" > "$tmp/audio_test.log" 2>&1; then
    pass "$(tail -1 "$tmp/audio_test.log")"
else
    fail "audio seam tests: $(grep -c '^FAIL' "$tmp/audio_test.log") failures"
    grep '^FAIL' "$tmp/audio_test.log" | head -10 >&2
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "audio leaf runner: ALL PASSED ($passes/$passes)"
    exit 0
else
    echo "audio leaf runner: $failures FAILURES" >&2
    exit 1
fi
