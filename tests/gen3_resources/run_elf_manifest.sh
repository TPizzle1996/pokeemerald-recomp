#!/usr/bin/env bash
#
# Stage R1A standalone test runner for tools/gen3_resources/elf_manifest.
#
# Build-isolated like the M0/M1 core runner: -std=c99 -Wall -Wextra -Werror
# plus the tool include root, the Gen3 core include root, and the shared core
# sources. No Emerald defines, no global.h/gba/SDL.
#
# Also verifies the checked-in manifest.generated.toml regenerates byte-for-byte
# via the CLI --check path against a freshly built fixture, and that the
# fixture itself reproduces deterministically.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
tool_dir="$root/tools/gen3_resources/elf_manifest"
core_dir="$root/src/gen3/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== building the tool + fixture_build (isolation flags) =="
make -C "$tool_dir" all

echo "== determinism: fixture_build reproduces the same bytes =="
"$tool_dir/fixture_build" --elf "$tmp/a.elf" --rom "$tmp/a.gba" > "$tmp/sha1.a"
"$tool_dir/fixture_build" --elf "$tmp/b.elf" --rom "$tmp/b.gba" > "$tmp/sha1.b"
if ! cmp -s "$tmp/a.elf" "$tmp/b.elf" || ! cmp -s "$tmp/a.gba" "$tmp/b.gba" \
   || ! cmp -s "$tmp/sha1.a" "$tmp/sha1.b"; then
    echo "FAIL: fixture_build is not deterministic" >&2
    exit 1
fi
echo "PASS: fixture_build deterministic"

echo "== --check: checked-in manifest regenerates byte-for-byte =="
fixture_sha1="$(awk '/^rom_sha1/{print $2}' "$tmp/sha1.a")"
"$tool_dir/gen3-elf-manifest" \
    --catalog "$root/resources/catalogs/emerald/catalog.toml" \
    --bindings "$root/resources/extraction/emerald/bpee01/bindings.toml" \
    --elf "$tmp/a.elf" \
    --rom "$tmp/a.gba" \
    --rom-sha1 "$fixture_sha1" \
    --qualification fixture \
    --provenance "generated from the synthetic bpee01 fixture (no retail ROM/ELF in this environment); regenerable via fixture_build + gen3-elf-manifest" \
    --check \
    --output "$root/resources/extraction/emerald/bpee01/manifest.generated.toml"

echo "== compiling R1A standalone tests =="
gcc -std=c99 -Wall -Wextra -Werror \
    -iquote "$tool_dir" \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    -DGEN3_FIXTURE_BUILD_NO_MAIN \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$core_dir/sha1.c" \
    "$tool_dir/elf_reader.c" \
    "$core_dir/toml.c" \
    "$tool_dir/manifest.c" \
    "$tool_dir/fixture_build.c" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_id.c" \
    "$here/elf_manifest_test.c" \
    -o "$tmp/elf_manifest_test"

echo "== running =="
cd "$root"
"$tmp/elf_manifest_test"
