#!/usr/bin/env bash
#
# Stage R2 resource-pack codec runner.
#
# Same build isolation as the M0/M1 core runner: -std=c99 -Wall -Wextra -Werror
# and the plain include paths only. NO Emerald defines, NO global.h/gba/SDL/
# platform objects. The codec (reader, writer, digests) is platform-neutral and
# is verified here against the independent vectors from
# tests/gen3_resources/resource_pack_vectors.py, not against itself.
#
# A grep assertion (same guardrail 18 pattern as run.sh) fails the run if any
# codec source/header ever gains an include that reaches into the Emerald/SDL
# frontend.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
core_dir="$root/src/gen3/resources"
inc_dir="$root/include/gen3/resources"

echo "== guardrail 18: dependency-creep assertion =="
bad=$(grep -rEn '^[[:space:]]*#[[:space:]]*include[[:space:]]+[<"][[:space:]]*(global\.h|gba|SDL|platform|graphics|sound|rom|main\.h)' \
      "$core_dir" "$inc_dir" || true)
if [ -n "$bad" ]; then
    echo "FAIL: shared gen3 core includes frontend dependencies:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "PASS: no global.h/gba/SDL/platform/rom includes in gen3 core"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling codec + tests (isolation flags) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_writer.c" \
    "$here/resource_pack_test.c" \
    -o "$tmp/gen3_resource_pack_test"

echo "== running =="
cd "$root"
"$tmp/gen3_resource_pack_test"
