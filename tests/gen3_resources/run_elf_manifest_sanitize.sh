#!/usr/bin/env bash
#
# Stage R1A ASan + UBSan run of the standalone generator tests. Same build
# isolation as run_elf_manifest.sh, plus the two sanitizers and leak detection.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
tool_dir="$root/tools/gen3_resources/elf_manifest"
core_dir="$root/src/gen3/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== building the tool + fixture_build (isolation flags) =="
make -C "$tool_dir" all

echo "== compiling R1A tests (ASan+UBSan) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
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
    -o "$tmp/elf_manifest_test_san"

echo "== running (detect_leaks=1) =="
cd "$root"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"$tmp/elf_manifest_test_san"
