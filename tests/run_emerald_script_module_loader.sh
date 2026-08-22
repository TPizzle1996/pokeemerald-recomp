#!/usr/bin/env bash
#
# R13-G2 §13 script-module loader/provider test
# (tests/emerald_script_module_loader_test.c).
#
# Pure pack-reader harness (no native emerald tables): the same
# resource_core/pack/toml/sha256 TUs the R6 runtime loader links, driven
# over the REAL production pack + the committed script-module artifacts:
#
#   1. pack identity (20,988 entries, BPEE01 Rev 0 ROM SHA-1);
#   2. all 467 payload-bearing script modules: found by canonical name,
#      byte-exact vs the committed .bin, payload SHA-256 == both manifest
#      digests;
#   3. the 56 routing-only modules are catalog identities only (absent
#      from the pack; the catalog is the full 523-identity surface) and
#      no pack script entry lies outside the catalog;
#   4. tamper refusal: payload-byte flip -> PAYLOAD_HASH_MISMATCH,
#      TOC-byte flip -> TOC_HASH_MISMATCH, truncated image -> refused.
#
# The ownership-state check (523 x ROM_BASE_ONLY since the R13-G6 sec 4
# flip; additive during G1-G5) runs in python after the C harness: the
# ownership file's [resources.targets] dotted headers are outside toml.c's
# supported TOML subset.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
mods="$root/resources/extraction/emerald/bpee01/script/modules"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling script module loader test (pack-reader harness) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra \
    -iquote include -iquote "$core_dir" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    -DDESKTOP_EXTERNAL_GAME_CONTENT \
    "$core_dir/sha256.c" \
    "$core_dir/sha1.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$here/emerald_script_module_loader_test.c" \
    -o "$tmp/emerald_script_module_loader_test"

echo "== running =="
"$tmp/emerald_script_module_loader_test" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack" \
    "$mods/manifest.production.toml" \
    "$mods/catalog.generated.toml" \
    "$root" \
    "$tmp"

echo "== ownership state check (523 x ROM_BASE_ONLY) =="
python3 - "$mods/ownership.generated.toml" <<'EOF'
import sys, tomllib
doc = tomllib.load(open(sys.argv[1], "rb"))
recs = doc["resources"]
assert len(recs) == 523, f"ownership resources {len(recs)} != 523"
bad = [r["id"] for r in recs
       if r.get("ownership_state") != "ROM_BASE_ONLY"
       or r.get("targets", {}).get("native") != "ROM_BASE_ONLY"]
assert not bad, f"non-ROM_BASE_ONLY ownership states: {bad}"
print(f"ok: 523/523 ownership records ROM_BASE_ONLY (no compiled fallback)")
EOF
