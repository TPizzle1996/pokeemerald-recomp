#!/usr/bin/env bash
#
# Stage R3 end-to-end synthetic test runner for the local Emerald ROM import
# pipeline (tests/emerald_resource_import_test.c).
#
# Same build isolation as the R1A/R2 runners: -std=c99 -Wall -Wextra -Werror and
# the plain include roots only. NO Emerald defines, NO global.h/gba/SDL/
# platform objects. The importer and its dependencies are platform-neutral.
#
# The synthetic bpee01 fixture ROM is built in-process from the checked-in
# Brendan source artifacts (graphics/trainers/...). The retail ROM is never
# read, written, or referenced by path; the retail profile is used only to
# prove fail-closed rejection of fixture content.
#
# Guardrail 18 assertion (dependency-creep): the shared gen3 core and the
# importer must never gain an include that reaches into the Emerald/SDL
# frontend.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
inc_dir="$root/include/gen3/resources"
emerald_dir="$root/src/emerald/resources"

echo "== guardrail 18: dependency-creep assertion =="
# emerald_trainer_native_compat.c and emerald_pokemon_native_compat.c are
# deliberately platform-coupled (guarded
# PLATFORM_SDL2 && NATIVE_LINUX, includes global.h) and is not compiled by
# this suite; it is exercised by run_emerald_trainer_native_compat.sh.
bad=$(grep -rEn '^[[:space:]]*#[[:space:]]*include[[:space:]]+[<"][[:space:]]*(global\.h|gba|SDL|platform|graphics|sound|rom|main\.h)' \
      --exclude='emerald_trainer_native_compat.c' \
      --exclude='emerald_pokemon_native_compat.c' \
      "$core_dir" "$inc_dir" "$emerald_dir" "$here/emerald_resource_import_test.c" || true)
if [ -n "$bad" ]; then
    echo "FAIL: gen3 core / importer includes frontend dependencies:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "PASS: no global.h/gba/SDL/platform/rom includes in gen3 core + importer"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling importer + dependencies + tests (isolation flags) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    -iquote "$root/tools/gen3_resources/elf_manifest" \
    -DGEN3_FIXTURE_BUILD_NO_MAIN \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$core_dir/sha1.c" \
    "$core_dir/sha256.c" \
    "$core_dir/toml.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_writer.c" \
    "$root/tools/gen3_resources/elf_manifest/fixture_build.c" \
    "$emerald_dir/emerald_rom_profile.c" \
    "$emerald_dir/emerald_resource_import.c" \
    "$here/emerald_resource_import_test.c" \
    -o "$tmp/emerald_resource_import_test"

echo "== running =="
cd "$root"
"$tmp/emerald_resource_import_test" "$tmp/scratch"
