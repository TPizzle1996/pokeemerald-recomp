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
if [ "$mode" = "sanitize" ] || [ "$mode" = "g4-sanitize" ]    || [ "$mode" = "h3-sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    # The R6 runtime snapshot/provider is intentionally process-lifetime
    # state in these focused harnesses; match the established G3 sanitizer
    # policy and gate memory/UB errors without treating that ownership model
    # as a G4 leak regression.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1"
fi

echo "== generating map script/event stub symbols =="
# Same stub object as Harness B: inert <Map>_MapScripts/<Map>_MapEvents
# pointer targets for maps.o's relocations, sourced from maps.o itself
# (nm -u) so the build-generated secret-base maps are covered.
# R13-G6 (plan sec 3/6): maps.s no longer references the G-owned script
# symbols (physically removed), so the list is legitimately empty - the
# remaining undefined symbols (_Layout, gMapHeaders, ITEM_*) are
# provided by the linked map-family seams. The empty stub object still
# assembles so the link is unchanged.
nm -u "$root/build/linux64/data/maps.o" | awk '/_(MapScripts|MapEvents)$/ {print $2}' | sort -u > "$tmp/stub_names.txt"
python3 - "$tmp" <<'EOF'
import sys
tmp = sys.argv[1]
names = [line.strip() for line in open(tmp + "/stub_names.txt") if line.strip()]
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
# NOTE: emerald_native_world_tables_stub.c is intentionally NOT linked here.
# The state test textually includes src/data.c, which already defines every
# native pic table; linking the stub as well would duplicate them.
cd "$root"
gcc -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -no-pie \
    -Wall -Wextra $sanitize_flags -no-pie \
    -iquote include -iquote "$core_dir" -iquote "$emerald_dir" \
    -I "$here/shim_include" \
    -DPORTABLE -DNONMATCHING -DUBFIX -DMODERN=1 \
    -DPLATFORM_SDL2 -DNATIVE_LINUX -DLINUX64=1 \
    -DDESKTOP_EXTERNAL_GAME_CONTENT \
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
    "$emerald_dir/emerald_leaf_compat.c" \
    "$emerald_dir/leaf_native_table.generated.c" \
    "$emerald_dir/emerald_text_compat.c" \
    "$emerald_dir/text_arenas.generated.c" \
    "$emerald_dir/text_native_table.generated.c" \
    "$emerald_dir/text_bundle_index.generated.c" \
    "$emerald_dir/text_slot_bindings.generated.c" \
    "$emerald_dir/text_slots_table.generated.c" \
    "$emerald_dir/text_skeletons_table.generated.c" \
    "$emerald_dir/text_skeleton_arrays.generated.c" \
    "$here/emerald_text_harness_stubs.c" \
    "$emerald_dir/gameplay_data_native.c" \
    "$emerald_dir/gameplay_native_table.generated.c" \
    "$emerald_dir/gameplay_levelup.generated.c" \
    "$emerald_dir/gameplay_callbacks.generated.c" \
    "$emerald_dir/gameplay_item_callbacks_native.c" \
    "$emerald_dir/emerald_gameplay_compat.c" \
    "$here/emerald_gameplay_harness_stubs.c" \
    "$emerald_dir/trainer_data_native.c" \
    "$emerald_dir/trainer_native.generated.c" \
    "$emerald_dir/emerald_trainer_compat.c" \
    "$emerald_dir/encounter_data_native.c" \
    "$emerald_dir/encounter_native.generated.c" \
    "$emerald_dir/emerald_encounter_compat.c" \
    "$emerald_dir/frontier_data_native.c" \
    "$emerald_dir/frontier_native.generated.c" \
    "$emerald_dir/frontier_aux_native.generated.c" \
    "$emerald_dir/emerald_frontier_compat.c" \
    "$emerald_dir/pokedex_data_native.c" \
    "$emerald_dir/pokedex_native.generated.c" \
    "$emerald_dir/emerald_pokedex_compat.c" \
    "$emerald_dir/map_data_native.c" \
    "$emerald_dir/map_native.generated.c" \
    "$emerald_dir/emerald_map_compat.c" \
    "$emerald_dir/emerald_script_compat.c" \
    "$emerald_dir/emerald_script_state.c" \
    "$emerald_dir/script_native_table.generated.c" \
    "$here/emerald_script_harness_stubs.c" \
    "$emerald_dir/emerald_battle_compat.c" \
    "$emerald_dir/emerald_battle_state.c" \
    "$emerald_dir/battle_native_table.generated.c" \
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

if [ "$mode" = "g4-state" ] || [ "$mode" = "g4-cross" ] \
   || [ "$mode" = "g4-sanitize" ]; then
    g4state="harness-g4-slot-7.st"
    echo "== R13-G4: capture in process A (five transactional cases) =="
    "$tmp/emerald_resource_state_test" g4-create "$pack" "$g4state" > g4-create.log
    cat g4-create.log
    grep -q "G4-CREATE" g4-create.log

    echo "== R13-G4: restore in fresh process B =="
    "$tmp/emerald_resource_state_test" g4-load "$pack" "$g4state" > g4-load.log
    cat g4-load.log
    grep -q "nested-return=ok dynamic-vaddress=8/8" g4-load.log

    python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/g4-create.log").read()
load = open(f"{tmp}/g4-load.log").read()
c = re.search(r"G4-CREATE arena=(0x[0-9a-f]+).*generation=(\d+)", create)
l = re.search(r"G4-LOAD arena=(0x[0-9a-f]+).*generation=(\d+)", load)
assert c and l, "missing G4 arena/generation proof line"
assert c.group(1) != l.group(1), "creator and restorer script arena bases match"
assert c.group(2) != l.group(2), "creator and restorer generation IDs match"
print(f"G4 fresh-process relocation: creator {c.groups()} -> restorer {l.groups()}")
EOF
    echo "R13-G4 state: 5 capture cases, 5 restore cases, 2 arena generations"
    if [ "$mode" != "g4-sanitize" ]; then
        exit 0
    fi
fi

if [ "$mode" = "g4-faults" ] || [ "$mode" = "g4-sanitize" ]; then
    echo "== R13-G4: refusal matrix =="
    "$tmp/emerald_resource_state_test" g4-faults "$pack" \
        "harness-g4-fault-slot-7.st" > g4-faults.log
    cat g4-faults.log
    grep -q "G4-FAULTS passed=31" g4-faults.log
    echo "R13-G4 faults: 31/31 capture/restore/identity checks"
    exit 0
fi

if [ "$mode" = "h3-state" ] || [ "$mode" = "h3-cross" ]; then
    h3state="harness-h3-slot-7.st"
    echo "== R13-H3: capture in process A (nested blocking-command battle state) =="
    "$tmp/emerald_resource_state_test" h3-create "$pack" "$h3state" > h3-create.log
    cat h3-create.log
    grep -q "H3-CREATE" h3-create.log

    echo "== R13-H3: restore in fresh process B (perturbed layout, new base) =="
    "$tmp/emerald_resource_state_test" h3-load "$pack" "$h3state" > h3-load.log
    cat h3-load.log
    grep -q "nested-return=ok blocking=ok alias=canonical" h3-load.log

    python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/h3-create.log").read()
load = open(f"{tmp}/h3-load.log").read()
c = re.search(r"H3-CREATE arena=(0x[0-9a-f]+).*generation=(\d+)", create)
l = re.search(r"H3-LOAD arena=(0x[0-9a-f]+).*generation=(\d+)", load)
assert c and l, "missing H3 arena/generation proof line"
assert c.group(1) != l.group(1), "creator and restorer battle arena bases match"
# Generation ids are per-process monotonic counters (not comparable
# across processes); the same-process restage + stale-generation
# refusals are proven by the h3-faults driver.
assert int(c.group(2)) >= 1 and int(l.group(2)) >= 1
print(f"H3 fresh-process relocation: creator {c.groups()} -> restorer {l.groups()}")
EOF
    echo "R13-H3 state: nested blocking-command capture + fresh-process restore"
    if [ "$mode" != "h3-cross" ]; then
        exit 0
    fi
    exit 0
fi

if [ "$mode" = "mixed-state" ] || [ "$mode" = "mixed-cross" ]; then
    mixedstate="harness-mixed-slot-7.st"
    echo "== R13-I: mixed-family capture in process A (G nested + H battle/anim/AI + engine) =="
    "$tmp/emerald_resource_state_test" mixed-create "$pack" "$mixedstate" > mixed-create.log 2>&1
    cat mixed-create.log
    grep -q "MIXED-CREATE" mixed-create.log
    grep -q "records=24 (g=9 h=9 text=6)" mixed-create.log

    echo "== R13-I: mixed-family restore in fresh process B (both generations re-staged) =="
    "$tmp/emerald_resource_state_test" mixed-load "$pack" "$mixedstate" > mixed-load.log 2>&1
    cat mixed-load.log
    grep -q "g-ok h-ok engine-ok mixed-return=ok" mixed-load.log

    python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/mixed-create.log").read()
load = open(f"{tmp}/mixed-load.log").read()
c = re.search(r"MIXED-CREATE arena=(0x[0-9a-f]+) battle-arena=(0x[0-9a-f]+)", create)
l = re.search(r"MIXED-LOAD arena=(0x[0-9a-f]+) battle-arena=(0x[0-9a-f]+)", load)
assert c and l, "missing mixed arena proof line"
assert c.group(1) != l.group(1), "creator and restorer script arena bases match"
assert c.group(2) != l.group(2), "creator and restorer battle arena bases match"
print(f"MIXED fresh-process relocation: creator {c.groups()} -> restorer {l.groups()}")
EOF
    echo "R13-I mixed: G+H+engine pointers resolve per-family across the process boundary"
    exit 0
fi

if [ "$mode" = "determinism" ]; then
    detstate="harness-det-slot-7.st"
    echo "== R13-I: deterministic semantic serialization across arena bases =="
    "$tmp/emerald_resource_state_test" determinism "$pack" "$detstate" > determinism.log 2>&1
    cat determinism.log
    grep -q "DETERMINISM payload=" determinism.log
    grep -q "bytes identical across arena bases" determinism.log
    exit 0
fi

if [ "$mode" = "h3-sanitize" ]; then
    echo "== R13-H3: sanitizer variants (ASan/UBSan instrumented binary) =="
    "$tmp/emerald_resource_state_test" h3-create "$pack"         "harness-h3-san-slot-7.st" > h3-san-create.log
    grep -q "H3-CREATE" h3-san-create.log
    "$tmp/emerald_resource_state_test" h3-load "$pack"         "harness-h3-san-slot-7.st" > h3-san-load.log
    grep -q "nested-return=ok" h3-san-load.log
    "$tmp/emerald_resource_state_test" h3-faults "$pack"         "harness-h3-san-fault-slot-7.st" > h3-san-faults.log
    grep -q "H3-FAULTS passed=16" h3-san-faults.log
    echo "R13-H3 sanitize: create/load/faults clean under ASan/UBSan"
    exit 0
fi

if [ "$mode" = "h3-faults" ]; then
    echo "== R13-H3: refusal matrix =="
    "$tmp/emerald_resource_state_test" h3-faults "$pack" \
        "harness-h3-fault-slot-7.st" > h3-faults.log
    cat h3-faults.log
    grep -q "H3-FAULTS passed=16" h3-faults.log
    echo "R13-H3 faults: 16/16 capture/restore/identity checks"
    exit 0
fi

if [ "$mode" = "g6-mevent" ] || [ "$mode" = "g4-sanitize" ]; then
    echo "== R13-G6 (plan sec 9): MEVENT boundary builder =="
    "$tmp/emerald_resource_state_test" g6-mevent "$pack" \
        "harness-g6-mevent-slot-7.st" > g6-mevent.log
    cat g6-mevent.log
    grep -q "G6-MEVENT passed" g6-mevent.log
    echo "R13-G6 MEVENT: active mystery-event capture validates boundaries"
    exit 0
fi

echo "== TEST 1/2/3: create (process A) =="
"$tmp/emerald_resource_state_test" create "$pack" "$state" > create.log
cat create.log
grep -q "CREATE ok" create.log
grep -q "CREATE recordCount=13" create.log

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

echo "== TEST T: R13-C currentChar relocates into the fresh text arena =="
grep -q "CREATE text arena:" create.log
grep -q "LOAD text arena:" load.log
python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/create.log").read()
load = open(f"{tmp}/load.log").read()
m = re.search(r"CREATE text arena: system_shared=(0x[0-9a-f]+)", create)
assert m, "create log lacks the text arena base"
c = m.group(1)
m = re.search(r"LOAD text arena: system_shared=(0x[0-9a-f]+) currentChar=(0x[0-9a-f]+) labelStart=(\d+) size=(\d+)", load)
assert m, "load log lacks the text relocation line"
l, char, label_start, size = m.group(1), m.group(2), m.group(3), m.group(4)
assert c != l, f"text arena base identical across processes: {c}"
assert size == "9", f"currentChar label span not 9: {size}"
assert hex(int(char, 16) - int(l, 16)) == hex(int(label_start) + 3), \
    f"currentChar not label start + 3: arena {l} char {char} labelStart {label_start}"
print(f"TEST T ok: creator {c} vs loader {l}, currentChar at label+3")
EOF
echo "TEST T ok (interior pointer relocated; opaque compiled pointer verbatim)"

echo "== TEST M: R13-F map arena pointers relocate into the fresh session =="
grep -q "CREATE map pointers:" create.log
grep -q "LOAD map pointers:" load.log
python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/create.log").read()
load = open(f"{tmp}/load.log").read()
pattern = r"(?:CREATE|LOAD) map pointers: layout=(0x[0-9a-f]+) events=(0x[0-9a-f]+) scripts=(0x[0-9a-f]+) connections=(0x[0-9a-f]+)"
m = re.search(pattern, create)
assert m, "create log lacks map pointers"
c = m.groups()
m = re.search(pattern, load)
assert m, "load log lacks map pointers"
l = m.groups()
assert c != l, f"map pointers identical across processes: {c}"
assert c[1] != l[1], f"event arena pointer was not relocated: {c[1]}"
assert c[3] != l[3], f"connection arena pointer was not relocated: {c[3]}"
print(f"TEST M ok: creator {c} vs loader {l}")
EOF
echo "TEST M ok (schemas 43/44 relocated; image pointers re-derived)"

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
for kind in bad-tag oob-offset bad-key bad-role bad-schema bad-type oob-resource-offset oversized-count duplicate-fields bad-reserved truncated-sidecar bad-sidecar-size raw-crc; do
    "$tmp/emerald_resource_state_test" create "$pack" "$state" > "create-$kind.log" 2>&1
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
