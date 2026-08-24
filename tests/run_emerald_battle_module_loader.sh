#!/usr/bin/env bash
#
# R13-H2 §25 battle-module loader/provider test
# (tests/emerald_battle_module_loader_test.c).
#
# Pure pack-reader harness (no native emerald tables): the same
# resource_core/pack/toml/sha256 TUs the R6 runtime loader links, driven
# over the REAL production pack + the committed battle-module artifacts:
#
#   1. pack identity (23,069 entries, BPEE01 Rev 0 ROM SHA-1);
#   2. all 2,081 payload-bearing battle modules: found by canonical name,
#      byte-exact vs the committed .bin, payload SHA-256 == both manifest
#      digests;
#   3. the 8 zero-width alias modules (4 battle-script + 4 battle-anim-script
#      identities sharing a base module's address) are catalog identities
#      only (absent from the pack; the catalog is the full 2,089-identity
#      surface) and no pack battle entry lies outside the catalog;
#   4. tamper refusal: payload-byte flip -> PAYLOAD_HASH_MISMATCH,
#      TOC-byte flip -> TOC_HASH_MISMATCH, truncated image -> refused.
#
# The ownership-state check (2,089 x ROM_BASE_ONLY since R13-H7's physical
# removal; gba target COMPILED) runs in python after the C harness: the
# ownership file's [resources.targets] dotted headers are outside toml.c's
# supported TOML subset.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
mods="$root/resources/extraction/emerald/bpee01/battle/modules"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== compiling battle module loader test (pack-reader harness) =="
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
    "$here/emerald_battle_module_loader_test.c" \
    -o "$tmp/emerald_battle_module_loader_test"

echo "== running =="
"$tmp/emerald_battle_module_loader_test" \
    "$root/games/emerald/base/emerald-bpee01-v1.rpack" \
    "$mods/manifest.production.toml" \
    "$mods/catalog.generated.toml" \
    "$root" \
    "$tmp"

echo "== ownership state check (2089 x ROM_BASE_ONLY) =="
python3 - "$mods/ownership.generated.toml" "$mods/catalog.generated.toml" <<'EOF'
import sys, tomllib
from collections import Counter
doc = tomllib.load(open(sys.argv[1], "rb"))
recs = doc["resources"]
assert len(recs) == 2089, f"ownership resources {len(recs)} != 2089"
bad = [r["id"] for r in recs
       if r.get("ownership_state") != "ROM_BASE_ONLY"
       or r.get("targets", {}).get("native") != "ROM_BASE_ONLY"
       or r.get("targets", {}).get("gba") != "COMPILED"]
assert not bad, f"non-ROM_BASE_ONLY ownership states: {bad}"
print("ok: 2089/2089 ownership records ROM_BASE_ONLY "
      "(gba target COMPILED, compiled payloads removed from native)")

# Per-family schema pins (schemas 47-51).
cat = tomllib.load(open(sys.argv[2], "rb"))
by_family = Counter()
schemas = {}
for r in cat["resources"]:
    fam = r["id"].rsplit("/", 1)[0]
    by_family[fam] += 1
    schemas.setdefault(fam, set()).add(r["schema"])
expected = {
    "emerald:battle-script": (645, {47}),
    "emerald:battle-anim-script": (658, {48}),
    "emerald:battle-ai": (553, {49}),
    "emerald:contest-ai": (165, {50}),
    "emerald:field-effect-script": (68, {51}),
}
assert dict(by_family) == {k: v for k, (v, _) in expected.items()}, \
    f"family counts {dict(by_family)}"
for fam, (_, want_schemas) in expected.items():
    assert schemas[fam] == want_schemas, \
        f"family {fam} schemas {schemas[fam]} != {want_schemas}"
print("ok: family counts 645/658/553/165/68 = 2089, schemas 47/48/49/50/51")
EOF
