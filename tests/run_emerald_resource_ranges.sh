#!/usr/bin/env bash
#
# R10-C: reverse resource-range index tests (tests/emerald_resource_ranges_test.c).
#
# Compiles the range index against a real EmeraldResourceCompatibilityImage
# (the same literal-only LZ77 path the seams use) and pins the lookup
# contract: range start / interior / last byte hit, one before/after miss,
# overlap rejection, overflow rejection, hull membership, identity round trip.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mode="${1:-}"
sanitize_flags=""
if [ "$mode" = "sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
fi

echo "== compiling resource range index test =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra $sanitize_flags \
    -iquote include -iquote "$core_dir" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    "$core_dir/sha256.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$emerald_dir/emerald_resource_ranges.c" \
    "$here/emerald_resource_ranges_test.c" \
    -o "$tmp/emerald_resource_ranges_test"

echo "== running =="
"$tmp/emerald_resource_ranges_test"
