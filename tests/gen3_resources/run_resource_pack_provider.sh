#!/usr/bin/env bash
#
# Stage R4 shared pack->provider adapter runner.
#
# Same build isolation as the M0/M1 and R2 runners: -std=c99 -Wall -Wextra
# -Werror and the plain include roots only. NO Emerald defines, NO
# global.h/gba/SDL/platform objects. The adapter (resource_pack_provider.c) is
# platform-neutral; the test asserts its behavior with `gen3:`-namespaced
# fixtures and no Emerald/BPEE01/Brendan/FireRed constants.
#
# Guardrail 18 assertion (dependency-creep) is the same pattern as run.sh.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
core_dir="$root/src/gen3/resources"
inc_dir="$root/include/gen3/resources"

echo "== guardrail 18: dependency-creep assertion =="
bad=$(grep -rEn '^[[:space:]]*#[[:space:]]*include[[:space:]]+[<"][[:space:]]*(global\.h|gba|SDL|platform|graphics|sound|rom|main\.h)' \
      "$core_dir" "$inc_dir" "$here/resource_pack_provider_test.c" || true)
if [ -n "$bad" ]; then
    echo "FAIL: shared gen3 core includes frontend dependencies:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "PASS: no global.h/gba/SDL/platform/rom includes in gen3 core + adapter"

echo "== guardrail 21: shared adapter carries no emerald constants =="
# Only genuine leak signatures count: macros (EMERALD_*), string literals
# ("emerald.rom-base.bpee01"), trainer-table symbols (gTrainer*), and identifier
# usage (BrendanFrontPic, FireRedGame). Doc-comment prose that names the things
# the adapter must NOT know (e.g. "...knows nothing about Emerald, BPEE01,
# Brendan, FireRed...") is not leakage and is excluded.
bad=$(grep -inE 'EMERALD_|"[^"]*(emerald|bpee|brendan|firered|trainer)|gTrainer|(emerald|bpee|brendan|firered|trainer)[A-Za-z]' \
      "$core_dir/resource_pack_provider.c" "$inc_dir/resource_pack_provider.h" || true)
if [ -n "$bad" ]; then
    echo "FAIL: shared adapter contains Emerald/BPEE01/Brendan/trainer/FireRed:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "PASS: shared adapter contains no Emerald/BPEE01/Brendan/trainer/FireRed"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling adapter + dependencies + tests (isolation flags) =="
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
    "$core_dir/resource_pack_provider.c" \
    "$here/resource_pack_provider_test.c" \
    -o "$tmp/gen3_resource_pack_provider_test"

echo "== running =="
cd "$root"
"$tmp/gen3_resource_pack_provider_test"
