#!/usr/bin/env bash
#
# Stage R4 Emerald ROM_BASE session runner (tests/emerald_rom_base_provider_test.c).
#
# Same build isolation as the R2/R3 runners: -std=c99 -Wall -Wextra -Werror and
# the plain include roots only. NO Emerald defines, NO global.h/gba/SDL/
# platform objects. The session helper, the shared adapter, and the gen3 core
# are all platform-neutral.
#
# The proof pack is built in-process from in-memory synthetic payloads using the
# R2 writer and the Emerald synthetic ROM profile. The retail ROM is never
# read, written, or referenced by path.
#
# Two guardrails:
#   - guardrail 18 (dependency-creep): neither the shared gen3 core, the shared
#     adapter, nor the Emerald session helper may include frontend headers.
#   - guardrail 21 (FireRed/emerald reuse): the SHARED adapter must not contain
#     Emerald/BPEE01/Brendan/trainer/FireRed constants - only the emerald files
#     may carry them (R4 §21).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
inc_dir="$root/include/gen3/resources"
emerald_dir="$root/src/emerald/resources"

echo "== guardrail 18: dependency-creep assertion =="
# emerald_trainer_native_compat.c is deliberately platform-coupled (guarded
# PLATFORM_SDL2 && NATIVE_LINUX, includes global.h) and is not compiled by
# this suite; it is exercised by run_emerald_trainer_native_compat.sh.
bad=$(grep -rEn '^[[:space:]]*#[[:space:]]*include[[:space:]]+[<"][[:space:]]*(global\.h|gba|SDL|platform|graphics|sound|rom|main\.h)' \
      --exclude='emerald_trainer_native_compat.c' \
      "$core_dir" "$inc_dir" "$emerald_dir" "$here/emerald_rom_base_provider_test.c" || true)
if [ -n "$bad" ]; then
    echo "FAIL: gen3 core / adapter / session includes frontend dependencies:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "PASS: no global.h/gba/SDL/platform/rom includes in gen3 core + adapter + session"

echo "== guardrail 21: shared adapter carries no emerald constants =="
# Only genuine leak signatures count: macros (EMERALD_*), string literals
# ("emerald.rom-base.bpee01"), trainer-table symbols (gTrainer*), and identifier
# usage (BrendanFrontPic, FireRedGame). The adapter's doc comments may NAME the
# things it must not know (e.g. "...knows nothing about Emerald, BPEE01, Brendan,
# FireRed...") - those are prose, not leakage, and are excluded by requiring the
# word to be followed by an identifier char, a quote, or an underscore.
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

echo "== compiling session + adapter + dependencies + tests (isolation flags) =="
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
    "$emerald_dir/emerald_rom_profile.c" \
    "$emerald_dir/emerald_resource_session.c" \
    "$here/emerald_rom_base_provider_test.c" \
    -o "$tmp/emerald_rom_base_provider_test"

echo "== running =="
cd "$root"
"$tmp/emerald_rom_base_provider_test"
