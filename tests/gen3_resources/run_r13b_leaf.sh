#!/usr/bin/env bash
#
# R13-B leaf payload ownership migration test runner.
#
#   E0   generator determinism: gen_leaf_family.py --check regenerates ALL
#        leaf-family outputs byte-identically (movement + multiboot
#        ownership/bindings/catalogs/leaf artifacts/seam table).
#   E1   pack provenance/determinism: gen3-pack-build --check reproduces
#        the committed production pack byte-for-byte from the 11 manifests
#        + 11 catalogs (the 17525-entry pack).
#   E1b  pack provenance mismatch (R13-B failure-matrix case): a movement
#        manifest whose rom_sha1 no longer matches the qualified profile
#        must fail the import - the manifest -> ROM digest validation is
#        the provenance gate the seam cannot see (the seam has no ROM
#        bytes).
#   E2   leaf manifest provenance: gen3-elf-manifest --check re-derives
#        the movement + multiboot manifests from the qualified ELF +
#        retail-matching ROM (three-way ELF == ROM == manifest canonical
#        slices, byte-for-byte).
#   A-G  seam tests: tests/emerald_leaf_compat_test.c against the REAL
#        production pack - A counts (1055 movement + 2 multiboot, 17525
#        pack entries), B exact extraction (arena bytes == pack payloads,
#        spans == pack entries, slices disjoint), C publication (session
#        -> TryInitialize -> arena, queries, canary, invalid args,
#        clear/shutdown), D additive semantics (failed republish keeps the
#        published arena), E failure matrix (missing movement family /
#        one entry / multiboot family, wrong-type movement / multiboot,
#        truncated movement / multiboot, overlapping claimed ROM slice,
#        pack duplicate-key guard).
#   F    native isolation battery: tests/run_emerald_native_asset_isolation.sh
#        --build against the ownership union (5819 ROM_BASE_ONLY
#        + 1057 COMPILED_PENDING_MIGRATION; the leaf movement symbols are
#        LOCAL, so the COMPILED presence check runs against the full nm
#        table).
#
# Build-isolated like the other Gen3 runners: gen3 core + session + seam
# only, no SDL, never launches the game.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"
movement="$root/resources/extraction/emerald/bpee01/movement"
multiboot="$root/resources/extraction/emerald/bpee01/multiboot"
text="$root/resources/extraction/emerald/bpee01/text"
gameplay="$root/resources/extraction/emerald/bpee01/gameplay"
pack="$root/games/emerald/base/emerald-bpee01-v1.rpack"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

failures=0
passes=0
pass() { passes=$((passes+1)); echo "  PASS $1"; }
fail() { failures=$((failures+1)); echo "  FAIL $1" >&2; }

echo "== E0: leaf generator determinism (gen_leaf_family.py --check) =="
if python3 "$root/tools/gen3_resources/leaf_family/gen_leaf_family.py" \
    . ../pokeemerald-reference/pokeemerald.elf \
    ../pokeemerald-reference/pokeemerald.gba \
    resources/extraction/emerald/bpee01 --check > "$tmp/gen_check.log" 2>&1; then
    pass "leaf family regenerates byte-identically (movement + multiboot)"
else
    fail "generator --check: $(tail -3 "$tmp/gen_check.log" | tr '\n' ' ')"
fi

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
    --manifest "$root/resources/extraction/emerald/bpee01/audio/manifest.production.toml" \
    --manifest "$text/manifest.production.toml" \
    --manifest "$gameplay/manifest.production.toml" \
    --manifest "$movement/manifest.production.toml" \
    --manifest "$multiboot/manifest.production.toml" \
    --catalog "$root/resources/catalogs/emerald/catalog.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/object_event/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/tileset/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/layout/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/audio/catalog.generated.toml" \
    --catalog "$text/catalog.generated.toml" \
    --catalog "$gameplay/catalog.generated.toml" \
    --catalog "$movement/catalog.generated.toml" \
    --catalog "$multiboot/catalog.generated.toml" \
    --check > "$tmp/pack_check.log" 2>&1; then
    pass "pack reproduces byte-for-byte (17525 entries since R13-D1)"
else
    fail "pack --check: $(tail -3 "$tmp/pack_check.log" | tr '\n' ' ')"
fi

echo "== E1b: pack provenance mismatch (tampered movement manifest) =="
sed 's/rom_sha1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"/rom_sha1 = "0000000000000000000000000000000000000000"/' \
    "$movement/manifest.production.toml" > "$tmp/movement.tampered.toml"
if "$root/tools/gen3_resources/pack_build/gen3-pack-build" \
    --rom ../pokeemerald-reference/pokeemerald.gba \
    --output "$tmp/tampered.rpack" \
    --manifest "$root/resources/extraction/emerald/bpee01/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/object_event/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/tileset/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/layout/manifest.production.toml" \
    --manifest "$root/resources/extraction/emerald/bpee01/audio/manifest.production.toml" \
    --manifest "$text/manifest.production.toml" \
    --manifest "$gameplay/manifest.production.toml" \
    --manifest "$tmp/movement.tampered.toml" \
    --manifest "$multiboot/manifest.production.toml" \
    --catalog "$root/resources/catalogs/emerald/catalog.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/object_event/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/tileset/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/layout/catalog.generated.toml" \
    --catalog "$root/resources/extraction/emerald/bpee01/audio/catalog.generated.toml" \
    --catalog "$text/catalog.generated.toml" \
    --catalog "$gameplay/catalog.generated.toml" \
    --catalog "$movement/catalog.generated.toml" \
    --catalog "$multiboot/catalog.generated.toml" \
    > "$tmp/tamper.log" 2>&1; then
    fail "tampered movement manifest accepted (provenance gate missed)"
else
    pass "tampered manifest refused: $(tail -1 "$tmp/tamper.log" | tr '\n' ' ')"
fi

echo "== E2: leaf manifest provenance (gen3-elf-manifest --check) =="
make -C "$root/tools/gen3_resources/elf_manifest" all >/dev/null
for family in movement multiboot; do
    dir="$root/resources/extraction/emerald/bpee01/$family"
    if "$root/tools/gen3_resources/elf_manifest/gen3-elf-manifest" \
        --catalog "$dir/catalog.generated.toml" \
        --bindings "$dir/bindings.generated.toml" \
        --elf ../pokeemerald-reference/pokeemerald.elf \
        --rom ../pokeemerald-reference/pokeemerald.gba \
        --rom-sha1 f3ae088181bf583e55daf962a92bb46f4f1d07b7 \
        --qualification production \
        --provenance "generated from the qualified pret reference build (d8e405c4f6b48f1faf3b26a3e045f0df2ff3ecb7 + upstream symbol rename b89722500) ELF + retail-matching recomp ROM; BPEE01 Rev 0 (SHA-1 f3ae088181bf583e55daf962a92bb46f4f1d07b7)" \
        --output "$dir/manifest.production.toml" \
        --check > "$tmp/manifest_$family.log" 2>&1; then
        pass "$family manifest re-derives byte-for-byte (ELF == ROM == manifest)"
    else
        fail "$family manifest --check: $(tail -3 "$tmp/manifest_$family.log" | tr '\n' ' ')"
    fi
done

echo "== A-G: leaf seam tests (emerald_leaf_compat_test.c) =="
gcc -std=gnu99 -O2 -iquote include -iquote gflib \
    -Wno-trigraphs -DNONMATCHING -DPORTABLE -DPLATFORM_SDL2 \
    -DRENDERER_EASY_DRAW -DMODERN=1 -DUBFIX -DDESKTOP_EXTERNAL_GAME_CONTENT \
    '-DEMERALD_EXPECTED_SHA1="0000000000000000000000000000000000000000"' \
    -DNATIVE_LINUX -DLINUX64=1 -fno-dce -fno-builtin -fno-pie \
    -c "$root/src/platform/host_memory.c" -o "$tmp/host_memory.o"
gcc -std=gnu99 -O2 -Wall -Wextra \
    -fno-pie -no-pie \
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
    "$emerald_dir/emerald_leaf_compat.c" \
    "$emerald_dir/leaf_native_table.generated.c" \
    "$tmp/host_memory.o" \
    "$root/tests/gen3_resources/host_memory_stubs.c" \
    "$root/tests/emerald_leaf_compat_test.c" \
    -o "$tmp/emerald_leaf_compat_test"
if "$tmp/emerald_leaf_compat_test" "$pack" "$tmp" > "$tmp/leaf_test.log" 2>&1; then
    pass "$(tail -1 "$tmp/leaf_test.log")"
else
    fail "leaf seam tests: $(grep -c '^FAIL' "$tmp/leaf_test.log") failures"
    grep '^FAIL' "$tmp/leaf_test.log" | head -10 >&2
fi

echo "== F: native isolation battery (6876-record ownership union) =="
if "$root/tests/run_emerald_native_asset_isolation.sh" --build \
    > "$tmp/isolation.log" 2>&1; then
    pass "$(tail -1 "$tmp/isolation.log")"
else
    fail "isolation battery: $(grep -c '^FAIL' "$tmp/isolation.log") failures"
    grep '^FAIL' "$tmp/isolation.log" | head -10 >&2
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "R13-B leaf runner: ALL PASSED ($passes/$passes)"
    exit 0
else
    echo "R13-B leaf runner: $failures FAILURES" >&2
    exit 1
fi
