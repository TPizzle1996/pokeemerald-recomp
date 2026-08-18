#!/usr/bin/env bash
#
# R10-G/H: cross-restart resource-pointer state tests
# (tests/emerald_resource_state_test.c + emerald_resource_state_stub.c).
#
# Compiles the REAL native_state.c + host_memory.c walker/restore machinery,
# the real compat seams, the real R6 runtime loader and the real src/data.c
# native branch against the REAL production pack, then drives the v5 state
# container across process boundaries:
#
#   TEST 1  same-content round trip (create then load, real published rows)
#   TEST 2  cross-restart: creator process exits; loader process is fresh
#   TEST 3  relocated resource allocation: arena bases provably differ
#   TEST 4  changed provider/session content identity -> transactional refuse
#   TEST 5  equivalent pack content at a different path -> loads
#   TEST 6  provider ordering (covered by run_emerald_session_fingerprint.sh)
#   TEST 7  missing session / unknown resource key -> refuse before mutation
#   TEST 8  corrupt sidecar matrix -> every case refuses safely
#   TEST H  real migrated battle resources: capture recognizes them, the
#           state carries sidecar references, load reconstructs pointers
#           (identity by key), parity follows from the real_tables chain
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"
pack="$root/games/emerald/base/emerald-bpee01-v1.rpack"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mode="${1:-}"
sanitize_flags=""
if [ "$mode" = "sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
fi

echo "== generating map script/event stub symbols =="
# Same stub object as Harness B: inert <Map>_MapScripts/<Map>_MapEvents
# pointer targets for maps.o's relocations, sourced from maps.o itself
# (nm -u) so the build-generated secret-base maps are covered.
nm -u "$root/build/linux64/data/maps.o" | awk '/_(MapScripts|MapEvents)$/ {print $2}' | sort -u > "$tmp/stub_names.txt"
python3 - "$tmp" <<'EOF'
import sys
tmp = sys.argv[1]
names = [line.strip() for line in open(tmp + "/stub_names.txt") if line.strip()]
assert names, "no <Map>_MapScripts/_MapEvents symbols found in maps.o"
with open(tmp + "/map_script_stubs.s", "w") as f:
    f.write("# R11-E/F Harness C: inert script/event pointer targets\n")
    f.write("# (from nm -u maps.o; never dereferenced by the module).\n")
    for name in sorted(names):
        f.write("    .balign 8\n")
        f.write("    .globl %s\n%s:\n" % (name, name))
        f.write("    .quad 0\n")
EOF
gcc -c "$tmp/map_script_stubs.s" -o "$tmp/map_script_stubs.o"

echo "== compiling cross-restart state test (real walker + seams + data.c + loader) =="
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wall -Wextra $sanitize_flags -no-pie \
    -iquote include -iquote "$core_dir" -iquote "$emerald_dir" \
    -I "$here/shim_include" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    "$core_dir/sha256.c" \
    "$core_dir/sha1.c" \
    "$core_dir/resource_lz.c" \
    "$core_dir/resource_id.c" \
    "$core_dir/resource_core.c" \
    "$core_dir/resource_resolver.c" \
    "$core_dir/resource_content_digest.c" \
    "$core_dir/resource_pack.c" \
    "$core_dir/resource_pack_provider.c" \
    "$core_dir/toml.c" \
    "$core_dir/util.c" \
    "$core_dir/lz77.c" \
    "$emerald_dir/emerald_rom_profile.c" \
    "$emerald_dir/emerald_resource_compat.c" \
    "$emerald_dir/emerald_resource_ranges.c" \
    "$emerald_dir/emerald_resource_session.c" \
    "$emerald_dir/emerald_trainer_native_compat.c" \
    "$emerald_dir/emerald_pokemon_native_compat.c" \
    "$emerald_dir/emerald_object_event_compat.c" \
    "$emerald_dir/emerald_tileset_compat.c" \
    "$emerald_dir/emerald_layout_compat.c" \
    "$emerald_dir/emerald_runtime_loader.c" \
    "$emerald_dir/emerald_audio_compat.c" \
    "$root/src/platform/native_state.c" \
    "$root/src/platform/host_memory.c" \
    "$root/src/platform/native_world_neighborhood.c" \
    "$root/src/platform/desktop_audio.c" \
    "$root/src/platform/desktop_state.c" \
    "$here/emerald_audio_device_shim.c" \
    "$root/build/linux64/data/maps.o" \
    "$tmp/map_script_stubs.o" \
    "$here/emerald_native_world_overworld_stub.c" \
    "$here/emerald_resource_state_stub.c" \
    "$here/emerald_resource_state_test.c" \
    -o "$tmp/emerald_resource_state_test"

cd "$tmp"
state="harness-state-slot-7.st"

echo "== TEST 1/2/3: create (process A) =="
"$tmp/emerald_resource_state_test" create "$pack" "$state" > create.log
cat create.log
grep -q "CREATE ok" create.log
grep -q "CREATE recordCount=10" create.log

echo "== TEST 1/2/3: load in a fresh process (process B) =="
"$tmp/emerald_resource_state_test" load "$pack" "$state" > load.log
cat load.log
grep -q "LOAD ok" load.log

echo "== TEST 3: arena bases differ between creator and loader =="
python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/create.log").read()
load = open(f"{tmp}/load.log").read()
m = re.search(r"CREATE arena pointers: front=(0x[0-9a-f]+) back=(0x[0-9a-f]+) trainer=(0x[0-9a-f]+)", create)
c = (m.group(1), m.group(2), m.group(3))
m = re.search(r"LOAD arena pointers: front=(0x[0-9a-f]+) back=(0x[0-9a-f]+) trainer=(0x[0-9a-f]+)", load)
l = (m.group(1), m.group(2), m.group(3))
assert c != l, f"arena bases identical across processes: {c}"
print(f"TEST 3 ok: creator {c} vs loader {l}")
EOF

echo "== TEST R: coincidental hull-band values capture as data =="
regstate="harness-regression-slot-7.st"
"$tmp/emerald_resource_state_test" regression "$pack" "$regstate" > regression.log
cat regression.log
grep -Eq "REGRESSION (ok|skipped)" regression.log
echo "TEST R ok (hull-band values in the unexposed prefix are data, not resource pointers)"

echo "== TEST S: typed sidecar scalar+padding windows are data (create) =="
scstate="harness-sidecar-slot-7.st"
"$tmp/emerald_resource_state_test" regression-sidecar "$pack" "$scstate" > sidecar-create.log
cat sidecar-create.log
grep -q "REGRESSION-SIDECAR create ok" sidecar-create.log
echo "TEST S create ok (SpriteTemplate/SpriteFrameImage scalar+padding heads are data)"

echo "== TEST S: typed sidecar members restore in a fresh process (load) =="
"$tmp/emerald_resource_state_test" load-sidecar "$pack" "$scstate" > sidecar-load.log
cat sidecar-load.log
grep -q "REGRESSION-SIDECAR load ok" sidecar-load.log
echo "TEST S load ok (genuine pointer members restored, scalar bytes round-trip verbatim)"

echo "== TEST A: audio-shaped game_bss state (create: mid-BGM/mid-cry arena pointers) =="
astate="harness-audio-slot-7.st"
"$tmp/emerald_resource_state_test" audio-regression "$pack" "$astate" > audio-create.log
cat audio-create.log
grep -q "AUDIO-REGRESSION ok" audio-create.log
echo "TEST A create ok (scalar windows verbatim, pointer fields tagged records, mid-BGM/mid-cry arena pointers keyed sidecar records)"

echo "== TEST A: audio fixture restores in a fresh process (load: ResolveByKey re-derivation) =="
"$tmp/emerald_resource_state_test" audio-load "$pack" "$astate" > audio-load.log
cat audio-load.log
grep -q "AUDIO-LOAD ok" audio-load.log
echo "TEST A load ok (mid-BGM/mid-cry pointers re-derived into the fresh arena via ResolveByKey)"

echo "== TEST A: audio arena bases differ between creator and loader (R12-E §9) =="
python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/audio-create.log").read()
load = open(f"{tmp}/audio-load.log").read()
m = re.search(r"CREATE audio arena: audio_vg=(0x[0-9a-f]+) audio_cry=(0x[0-9a-f]+)", create)
c = (m.group(1), m.group(2))
m = re.search(r"LOAD audio arena: audio_vg=(0x[0-9a-f]+) audio_cry=(0x[0-9a-f]+)", load)
l = (m.group(1), m.group(2))
assert c != l, f"audio arena bases identical across processes: {c}"
print(f"TEST A relocation ok: audio creator {c} vs loader {l}")
EOF

echo "== TEST B: desktop state-manager load sequence (real desktop_state.c + desktop_audio.c over the fake SDL device) =="
dstate="harness-desktop-slot-7.st"
"$tmp/emerald_resource_state_test" desktop-audio "$pack" "$dstate" > desktop-audio.log
cat desktop-audio.log
grep -q "DESKTOP-AUDIO ok" desktop-audio.log
echo "TEST B manager load ok (ordered device trace verbatim, PCM parity, device active/unpaused)"

echo "== TEST B: paused-at-load variant (Ctrl+P then state manager) =="
"$tmp/emerald_resource_state_test" desktop-audio-paused "$pack" "$dstate" > desktop-audio-paused.log
cat desktop-audio-paused.log
grep -q "DESKTOP-AUDIO ok" desktop-audio-paused.log
echo "TEST B paused variant ok (device.paused tracks host paused through the load)"

echo "== TEST B: quick-load path (no UI pause at all) =="
"$tmp/emerald_resource_state_test" desktop-quick "$pack" "$dstate" > desktop-quick.log
cat desktop-quick.log
grep -q "DESKTOP-QUICK ok" desktop-quick.log
echo "TEST B quick-load ok (device never paused; only the serializer tail's clear)"

echo "== TEST 5: equivalent content at a different path =="
cp "$pack" "$tmp/copied-pack.rpack"
"$tmp/emerald_resource_state_test" load "$tmp/copied-pack.rpack" "$state" > load2.log
grep -q "LOAD ok" load2.log
echo "TEST 5 ok (equivalent content, different path)"

echo "== TEST 4: changed session content identity refuses transactionally =="
"$tmp/emerald_resource_state_test" load-fail "$pack" "$state" fingerprint > fail4.log
cat fail4.log
grep -q "LOADFAIL fingerprint ok" fail4.log
echo "TEST 4 ok"

echo "== TEST 7: no active session refuses =="
"$tmp/emerald_resource_state_test" load-fail "$pack" "$state" missing-session > fail7.log
cat fail7.log
grep -q "LOADFAIL missing-session ok" fail7.log
echo "TEST 7a ok (missing session)"

echo "== TEST 7: unknown resource key refuses before mutation =="
python3 "$here/emerald_resource_state_corrupt.py" "$tmp/$state" corrupt-key
"$tmp/emerald_resource_state_test" load-fail "$pack" "$state" corrupt > fail7b.log
cat fail7b.log
grep -q "LOADFAIL corrupt ok" fail7b.log
echo "TEST 7b ok (unknown resource key)"

echo "== TEST 8: corrupt sidecar matrix =="
for kind in bad-tag oob-offset bad-key bad-role bad-schema oob-resource-offset oversized-count duplicate-fields bad-reserved truncated-sidecar; do
    "$tmp/emerald_resource_state_test" create "$pack" "$state" > /dev/null
    python3 "$here/emerald_resource_state_corrupt.py" "$tmp/$state" "$kind"
    "$tmp/emerald_resource_state_test" load-fail "$pack" "$state" corrupt > "fail8-$kind.log" || true
    if ! grep -q "LOADFAIL corrupt ok" "fail8-$kind.log"; then
        echo "FAIL: corrupt sidecar kind $kind did not reject safely:"
        cat "fail8-$kind.log"
        exit 1
    fi
    echo "TEST 8 $kind ok"
done

echo "== TEST I: v4 compatibility policy =="
"$tmp/emerald_resource_state_test" create "$pack" "$state" > /dev/null
python3 "$here/emerald_resource_state_corrupt.py" "$tmp/$state" v4-version
"$tmp/emerald_resource_state_test" load-fail "$pack" "$state" corrupt > faili1.log || true
grep -q "format v4 predates resource-aware serialization" faili1.log
echo "TEST I v4 rejection ok ($(grep -o 'format v4 predates.*' faili1.log))"

echo "== TEST I: unsupported version =="
"$tmp/emerald_resource_state_test" create "$pack" "$state" > /dev/null
python3 "$here/emerald_resource_state_corrupt.py" "$tmp/$state" unsupported-version
"$tmp/emerald_resource_state_test" load-fail "$pack" "$state" corrupt > faili2.log || true
grep -q "LOADFAIL corrupt ok" faili2.log
grep -q "state format version is not supported" faili2.log
echo "TEST I unsupported-version ok"

echo
echo "cross-restart resource state tests: ALL PASSED"
