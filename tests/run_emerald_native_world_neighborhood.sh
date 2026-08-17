#!/usr/bin/env bash
#
# R11-E Harness A: NativeWorldNeighborhood — hand-built fixtures
# (tests/emerald_native_world_neighborhood_test.c).
#
# Compiles the neighborhood module + host_memory.c (HostResolveGbaAddr /
# HostResolveGbaTableEntry / HostPointerToGbaAddr) + a stub
# Overworld_GetMapHeaderByGroupAndId whose body is the verbatim PORTABLE
# branch (overworld.c:595-603) over a fixture gMapGroups GbaAddr table, with
# hand-built MapHeader/MapLayout/MapEvents/connection records and a real
# `struct SaveBlock1` fixture. No seam code, no pack, no game data: pure math
# with fully controlled inputs.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mode="${1:-}"
sanitize_flags=""
if [ "$mode" = "sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
fi

echo "== compiling harness A (hand-built fixtures) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra $sanitize_flags -no-pie \
    -iquote include \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    src/platform/native_world_neighborhood.c \
    src/platform/host_memory.c \
    "$here/emerald_native_world_stub.c" \
    "$here/emerald_native_world_neighborhood_test.c" \
    -o "$tmp/emerald_native_world_neighborhood_test"

echo "== running =="
"$tmp/emerald_native_world_neighborhood_test"
