#!/usr/bin/env bash
#
# Stage M0/M1 shared-core build-isolation runner (guardrails 16, 18).
#
# Compiles the platform-neutral Gen3 resource core (sha256.c, resource_id.c,
# resource_core.c, resource_resolver.c) together with tests/.../resource_core_test.c
# using ONLY -std=c99 -Wall -Wextra -Werror and the plain include paths -- NO
# Emerald defines (-DPORTABLE, -DNONMATCHING, -DUBFIX, -DMODERN, -DPLATFORM_SDL2,
# -DNATIVE_LINUX, -DLINUX64), NO global.h, gba, SDL, desktop, or platform objects.
#
# A grep assertion (guardrail 18) fails the run if any gen3 core source/header
# ever gains an include that reaches into the Emerald/SDL frontend, so dependency
# creep is caught by the test suite, not just by the build working by luck.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
core_dir="$root/src/gen3/resources"
inc_dir="$root/include/gen3/resources"

echo "== guardrail 18: dependency-creep assertion =="
# Matches #include of the Emerald/SDL frontend or host platform. Deliberately
# does NOT match gen3's own internal includes (sha256.h, resource_internal.h,
# gen3/resources/*).
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

echo "== compiling shared core + tests (isolation flags) =="
gcc -std=c99 -Wall -Wextra -Werror \
    -iquote "$root/include" \
    -iquote "$core_dir" \
    "$core_dir/sha256.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$here/resource_core_test.c" \
    -o "$tmp/gen3_resource_core_test"

echo "== running =="
"$tmp/gen3_resource_core_test"
