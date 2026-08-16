#!/usr/bin/env bash
#
# Stage R5 codec + compatibility-image runner (tests/emerald_resource_lz_unit.c).
#
# Platform-neutral isolation, same as the R2/R3/R4 runners: -std=c99
# -Wall -Wextra -Werror and the plain include roots only. NO Emerald runtime
# defines, NO global.h/gba/SDL/platform objects. The reusable codec and the
# compat image are both frontend-free (guardrail 18); the GBA decode side in
# this suite is a faithful host port of the real decoder - the authoritative
# real-decoder round-trip lives in emerald_trainer_native_compat_test.c.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"

echo "== guardrail 18: dependency-creep assertion =="
bad=$(grep -rEn '^[[:space:]]*#[[:space:]]*include[[:space:]]+[<"][[:space:]]*(global\.h|gba|SDL|platform|graphics|sound|rom|main\.h)' \
      "$core_dir/resource_lz.c" "$core_dir/resource_core.c" \
      "$core_dir/resource_id.c" "$core_dir/resource_resolver.c" \
      "$core_dir/toml.c" "$core_dir/util.c" "$core_dir/sha256.c" \
      "$root/include/gen3/resources/resource_lz.h" \
      "$emerald_dir/emerald_resource_compat.c" "$root/include/emerald/resources/emerald_resource_compat.h" \
      "$here/emerald_resource_lz_unit.c" || true)
if [ -n "$bad" ]; then
    echo "FAIL: codec / compat image includes frontend dependencies:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "PASS: no global.h/gba/SDL/platform includes in codec + compat image + test"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling codec + compat image + tests (isolation flags) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$here/emerald_resource_lz_unit.c" \
    -o "$tmp/emerald_resource_lz_unit"

echo "== running =="
cd "$root"
"$tmp/emerald_resource_lz_unit"
