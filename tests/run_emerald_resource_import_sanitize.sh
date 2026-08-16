#!/usr/bin/env bash
#
# Stage R3 ASan + UBSan run of the end-to-end synthetic importer test
# (guardrail 30). Same isolation flags as run_emerald_resource_import.sh, plus
# the two sanitizers and leak detection.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling importer + dependencies + tests (ASan+UBSan) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
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
    -o "$tmp/emerald_resource_import_test_san"

echo "== running (detect_leaks=1) =="
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"$tmp/emerald_resource_import_test_san" "$tmp/scratch"
