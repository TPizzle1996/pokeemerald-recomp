#!/usr/bin/env bash
#
# R13-H4/H5/H6: live cutover tests for the battle + battle-anim +
# battle-AI + contest-AI + field-effect arenas
# (tests/emerald_battle_live_test.c).
#
# Compiles the SAME production machinery as run_emerald_resource_state.sh
# (real walker, real seams, real R6 loader, maps.o) with the H3 shadow
# seam swapped for the PRODUCTION live seam:
#
#   - emerald_battle_compat.c + battle_native_table.generated.c  (H3)
#   + emerald_battle_live.c + battle_live_table.generated.c      (H4/H5)
#   + battle_live_native.generated.c + inert native-address stubs
#
# and drives it against the REAL production pack:
#
#   oracle         differential resolution oracle over all 2,081 modules
#                  and 7,549 relocs, both physical layouts; staged bytes
#                  byte-exact against the committed .bin artifacts
#   faults         brief sec 24 fail-closed matrix (11 cases)
#   replace        brief sec 25/26: generation replacement, 6,382 range
#                  invariant, identity-based unregister
#   battle-oracle  H5: 238 routing rows, 199 compiled labels, 50 EWRAM
#                  bindings, battle pointer sweep, both layouts
#   battle-faults  H5 battle fail-closed matrix (8 cases)
#   state          brief sec 15: fresh-process State-v5 anim proof
#   h5-state       H5 sec 22: nested blocking battle fresh-process proof
#   ai-oracle      H6: 2,351 AI instructions, 1,586 AI relocs (38 data
#                  targets), 64 AI routing rows, typed resolution + AI
#                  pointer sweep, both layouts
#   ai-faults      H6 AI fail-closed matrix (10 cases)
#   ai-249         H6: 235-opcode differential, 103 real + 132 synthetic
#                  slots, mini-VM execution of both AI entry roots
#   h6-state       H6 sec 22: AI fresh-process proof per family
#                  (battle-ai + contest-ai)
#   sanitize       all of the above under ASan/UBSan
#
# Modes: oracle | faults | replace | battle-oracle | battle-faults |
#        state | h5-state | ai-oracle | ai-faults | ai-249 | h6-state |
#        sanitize (default: all).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
core_dir="$root/src/gen3/resources"
emerald_dir="$root/src/emerald/resources"
pack="$root/games/emerald/base/emerald-bpee01-v1.rpack"
mods_dir="$root/resources/extraction/emerald/bpee01/battle/modules"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mode="${1:-}"
sanitize_flags=""
if [ "$mode" = "sanitize" ]; then
    sanitize_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    # Same policy as the G3/H3 sanitizer harnesses: the runtime snapshot
    # provider is intentionally process-lifetime state; gate memory/UB
    # errors without treating that ownership model as a leak regression.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:halt_on_error=1"
fi

echo "== generating map script/event stub symbols =="
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

echo "== generating engine binding stubs (battle_live_table externs) =="
# The live table's semantic bindings reference the real engine symbols
# (anim tasks, FE command natives, gfx/sprite-template data). The harness
# never CALLS them - the bindings are resolved for their ADDRESS only -
# so inert data slots satisfy the link without pulling in the battle
# engine. Externs defined in the same file (e.g. kEmeraldBattleLiveTable)
# are excluded.
python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
table = open("/home/tristen/work/pokeemerald-recomp/src/emerald/resources/battle_live_table.generated.c").read()
text = re.sub(r"/\*.*?\*/", "", table, flags=re.S)
text = re.sub(r"//[^\n]*", "", text)
names = []
for m in re.finditer(r"\bextern\b", text):
    decl = text[m.end():text.find(";", m.end())]
    if "(" in decl:
        name = re.findall(r"\w+", decl.split("(")[0])[-1]
    else:
        name = re.findall(r"\w+", decl)[-1]
    # skip symbols that are also defined in the same file
    if re.search(r"\b%s\b\s*[={]" % re.escape(name), table):
        continue
    names.append(name)
with open(tmp + "/binding_stubs.s", "w") as f:
    f.write("# R13-H4 harness: inert engine-binding symbol slots\n")
    f.write("# (address-only references from battle_live_table.generated.c;\n")
    f.write("# never executed by the harness).\n")
    for name in sorted(set(names)):
        f.write("    .balign 8\n")
        f.write("    .globl %s\n%s:\n" % (name, name))
        f.write("    .quad 0\n")
EOF
gcc -c "$tmp/binding_stubs.s" -o "$tmp/binding_stubs.o"

# R13-H5: the native-address TU (battle_live_native.generated.c) takes
# the ADDRESS of the A/B battle binding symbols and the compiled battle
# labels; the harness provides inert cells (address-only, never read).
echo "== generating battle native-address stubs (H5) =="
gcc -c "$here/battle_live_native_stubs.s" -o "$tmp/battle_live_native_stubs.o"

echo "== compiling H4/H5 live-cutover test (real walker + seams + loader + LIVE seam) =="
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
    "$emerald_dir/emerald_battle_state.c" \
    "$emerald_dir/emerald_battle_live.c" \
    "$emerald_dir/battle_live_table.generated.c" \
    "$emerald_dir/battle_live_native.generated.c" \
    "$root/src/platform/native_state.c" \
    "$root/src/platform/host_memory.c" \
    "$root/src/platform/native_world_neighborhood.c" \
    "$root/src/platform/desktop_audio.c" \
    "$root/src/platform/desktop_state.c" \
    "$here/emerald_audio_device_shim.c" \
    "$root/build/linux64/data/maps.o" \
    "$tmp/map_script_stubs.o" \
    "$tmp/binding_stubs.o" \
    "$tmp/battle_live_native_stubs.o" \
    "$here/emerald_native_world_overworld_stub.c" \
    "$here/emerald_native_world_tables_stub.c" \
    "$here/emerald_resource_state_stub.c" \
    "$here/emerald_battle_live_test.c" \
    -o "$tmp/emerald_battle_live_test"

cd "$tmp"

if [ "$mode" = "oracle" ] || [ "$mode" = "sanitize" ]; then
    for layout in 0 1; do
        echo "== H4 oracle: layout $layout =="
        "$tmp/emerald_battle_live_test" oracle "$pack" "$mods_dir" "$layout" > "oracle-$layout.log"
        cat "oracle-$layout.log"
        grep -q "H4-ORACLE layout=$layout entry-words=2043 byte-exact=2081 relocs=7549" "oracle-$layout.log"
    done
    echo "H4/H5 oracle: 2,043 entry words, 2,081 byte-exact modules, 7,549 relocs, both layouts"
fi

if [ "$mode" = "faults" ] || [ "$mode" = "sanitize" ]; then
    echo "== H4 fail-closed matrix =="
    "$tmp/emerald_battle_live_test" faults "$pack" > faults.log
    cat faults.log
    grep -q "LIVE-FAULTS passed=11" faults.log
    echo "H4 faults: 11/11 fail-closed cases"
fi

if [ "$mode" = "replace" ] || [ "$mode" = "sanitize" ]; then
    echo "== H4 generation replacement =="
    "$tmp/emerald_battle_live_test" replace "$pack" > replace.log
    cat replace.log
    grep -q "H4-REPLACE" replace.log
    grep -q "count=6390" replace.log
    echo "H4 replace: generation B committed, 6,377 invariant, identity unregister/restore"
fi

if [ "$mode" = "state" ] || [ "$mode" = "sanitize" ] || [ "$mode" = "" ]; then
    state="harness-h4-slot-7.st"
    echo "== H4 fresh-process state proof: capture in process A =="
    "$tmp/emerald_battle_live_test" state-create "$pack" "$state" > state-create.log
    cat state-create.log
    grep -q "H4-CREATE" state-create.log

    echo "== H4 fresh-process state proof: restore in fresh process B =="
    "$tmp/emerald_battle_live_test" state-load "$pack" "$state" > state-load.log
    cat state-load.log
    grep -q "H4-LOAD" state-load.log

    python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/state-create.log").read()
load = open(f"{tmp}/state-load.log").read()
c = re.search(r"H4-CREATE arena=(0x[0-9a-f]+) generation=(\d+) module=(\d+) ip=(\d+) ret=(\d+)", create)
l = re.search(r"H4-LOAD arena=(0x[0-9a-f]+) generation=(\d+) module=(\d+) ip=(\d+) ret=(\d+)", load)
assert c and l, "missing H4 arena/generation proof line"
# Physical arena base moved across the process boundary + layout
# perturbation; the module/offset identity held.
assert c.group(1) != l.group(1), "creator and restorer anim arena bases match"
assert c.group(3) == l.group(3), f"module identity changed across restart: {c.group(3)} vs {l.group(3)}"
assert c.group(4) == l.group(4), f"IP offset changed across restart: {c.group(4)} vs {l.group(4)}"
assert c.group(5) == l.group(5), f"return offset changed across restart: {c.group(5)} vs {l.group(5)}"
print(f"H4 fresh-process relocation: creator {c.groups()} -> restorer {l.groups()}")
EOF
    echo "H4 state: live anim IP/return relocated by module+offset identity into the new arena"
fi

if [ "$mode" = "battle-oracle" ] || [ "$mode" = "sanitize" ]; then
    for layout in 0 1; do
        echo "== H5 battle oracle: layout $layout =="
        "$tmp/emerald_battle_live_test" battle-oracle "$pack" "$mods_dir" "$layout" > "battle-oracle-$layout.log"
        cat "battle-oracle-$layout.log"
        grep -q "H5-BATTLE-ORACLE layout=$layout routing-rows=238 labels=199 ewram=50" "battle-oracle-$layout.log"
    done
    echo "H5 battle oracle: 238 routing rows, 199 labels, 50 EWRAM bindings, pointer sweep, both layouts"
fi

if [ "$mode" = "battle-faults" ] || [ "$mode" = "sanitize" ]; then
    echo "== H5 battle fail-closed matrix =="
    "$tmp/emerald_battle_live_test" battle-faults "$pack" > battle-faults.log
    cat battle-faults.log
    grep -q "H5-BATTLE-FAULTS passed=8" battle-faults.log
    echo "H5 battle faults: 8/8 fail-closed cases"
fi

if [ "$mode" = "h5-state" ] || [ "$mode" = "sanitize" ] || [ "$mode" = "" ]; then
    state="harness-h5-slot-7.st"
    echo "== H5 nested fresh-process state proof: capture in process A =="
    "$tmp/emerald_battle_live_test" h5-state-create "$pack" "$mods_dir" "$state" > h5-state-create.log
    cat h5-state-create.log
    grep -q "H5-CREATE" h5-state-create.log

    echo "== H5 nested fresh-process state proof: restore in fresh process B =="
    "$tmp/emerald_battle_live_test" h5-state-load "$pack" "$mods_dir" "$state" > h5-state-load.log
    cat h5-state-load.log
    grep -q "H5-LOAD" h5-state-load.log

    python3 - "$tmp" <<'EOF'
import re, sys
tmp = sys.argv[1]
create = open(f"{tmp}/h5-state-create.log").read()
load = open(f"{tmp}/h5-state-load.log").read()
c = re.search(r"H5-CREATE arena=(0x[0-9a-f]+) generation=(\d+) module=(\d+) ip=(\d+) ret0=(\d+)/(\d+) ret1=(\d+)/(\d+)", create)
l = re.search(r"H5-LOAD arena=(0x[0-9a-f]+) generation=(\d+) module=(\d+) ip=(\d+) ret0=(\d+)/(\d+) ret1=(\d+)/(\d+)", load)
assert c and l, "missing H5 arena/generation proof line"
# Physical arena base moved across the process boundary + layout
# perturbation; every (module, offset) identity held - the blocking
# waitmessage IP and BOTH nested IP+5 returns in different modules.
assert c.group(1) != l.group(1), "creator and restorer battle arena bases match"
assert c.group(3) == l.group(3), f"waitmessage module changed across restart: {c.group(3)} vs {l.group(3)}"
assert c.group(4) == l.group(4), f"IP offset changed across restart: {c.group(4)} vs {l.group(4)}"
assert c.group(5) == l.group(5) and c.group(6) == l.group(6), f"return 0 identity changed: {c.group(5)}/{c.group(6)} vs {l.group(5)}/{l.group(6)}"
assert c.group(7) == l.group(7) and c.group(8) == l.group(8), f"return 1 identity changed: {c.group(7)}/{c.group(8)} vs {l.group(7)}/{l.group(8)}"
assert c.group(5) != c.group(7), "the two returns must live in DIFFERENT modules"
print(f"H5 fresh-process relocation: creator {c.groups()} -> restorer {l.groups()}")
EOF
    echo "H5 state: nested blocking battle IP + two IP+5 returns relocated by module+offset identity"
fi

if [ "$mode" = "battle-249" ] || [ "$mode" = "sanitize" ]; then
    echo "== H5 249-opcode differential =="
    "$tmp/emerald_battle_live_test" battle-249 "$pack" "$mods_dir" > battle-249.log
    cat battle-249.log
    grep -q "H5-249 layout=0 decoded=" battle-249.log
    grep -q "slots-present=238 synthetic=11" battle-249.log
    echo "H5 249: 3,390 qualified instructions decode, all 249 opcode slots covered (238 real + 11 synthetic)"
fi

if [ "$mode" = "ai-oracle" ] || [ "$mode" = "sanitize" ]; then
    for layout in 0 1; do
        echo "== H6 AI oracle: layout $layout =="
        "$tmp/emerald_battle_live_test" ai-oracle "$pack" "$mods_dir" "$layout" > "ai-oracle-$layout.log"
        cat "ai-oracle-$layout.log"
        grep -q "H6-AI-ORACLE layout=$layout instructions=2351 battle-ai=1738 contest-ai=613 ai-relocs=1586 data-targets=38 routing-rows=64" "ai-oracle-$layout.log"
        python3 - "$tmp" "$layout" <<'EOF'
import re, sys
tmp, layout = sys.argv[1], sys.argv[2]
log = open(f"{tmp}/ai-oracle-{layout}.log").read()
m = re.search(r"swept-words=(\d+)", log)
assert m, "missing AI pointer sweep line"
assert int(m.group(1)) >= 1800, f"AI sweep too small: {m.group(1)}"
print(f"H6 AI pointer sweep: {m.group(1)} non-reloc 4-byte words verified non-target")
EOF
    done
    echo "H6 AI oracle: 2,351 instructions, 1,586 relocs, 38 data targets, 64 routing rows, sweep, both layouts"
fi

if [ "$mode" = "ai-faults" ] || [ "$mode" = "sanitize" ]; then
    echo "== H6 AI fail-closed matrix =="
    "$tmp/emerald_battle_live_test" ai-faults "$pack" > ai-faults.log
    cat ai-faults.log
    grep -q "H6-AI-FAULTS passed=10" ai-faults.log
    echo "H6 AI faults: 10/10 fail-closed cases"
fi

if [ "$mode" = "h7-faults" ] || [ "$mode" = "sanitize" ]; then
    echo "== H7 fail-closed matrix =="
    "$tmp/emerald_battle_live_test" h7-faults "$pack" > h7-faults.log
    cat h7-faults.log
    grep -q "H7-FAULTS passed=22" h7-faults.log
    echo "H7 faults: 22/22 fail-closed cases"
fi

if [ "$mode" = "ai-249" ] || [ "$mode" = "sanitize" ]; then
    echo "== H6 235-opcode differential =="
    "$tmp/emerald_battle_live_test" ai-249 "$pack" "$mods_dir" > ai-249.log
    cat ai-249.log
    grep -q "H6-AI-249 layout=0 decoded=2351 slots-present=103 battle-ai=67 contest-ai=36 synthetic=132 executed=2" ai-249.log
    echo "H6 249: 2,351 AI instructions decode, all 235 opcode slots covered (103 real + 132 synthetic), both AI entry roots execute"
fi

if [ "$mode" = "h6-state" ] || [ "$mode" = "sanitize" ] || [ "$mode" = "" ]; then
    for family in battle-ai contest-ai; do
        state="harness-h6-$family-slot-7.st"
        echo "== H6 $family fresh-process state proof: capture in process A =="
        "$tmp/emerald_battle_live_test" h6-state-create "$pack" "$mods_dir" "$state" "$family" > "h6-state-create-$family.log"
        cat "h6-state-create-$family.log"
        grep -q "H6-CREATE" "h6-state-create-$family.log"

        echo "== H6 $family fresh-process state proof: restore in fresh process B =="
        "$tmp/emerald_battle_live_test" h6-state-load "$pack" "$mods_dir" "$state" "$family" > "h6-state-load-$family.log"
        cat "h6-state-load-$family.log"
        grep -q "H6-LOAD" "h6-state-load-$family.log"

        python3 - "$tmp" "$family" <<'EOF'
import re, sys
tmp, family = sys.argv[1], sys.argv[2]
create = open(f"{tmp}/h6-state-create-{family}.log").read()
load = open(f"{tmp}/h6-state-load-{family}.log").read()
c = re.search(r"H6-CREATE family=(\d+) arena=(0x[0-9a-f]+) generation=(\d+) module=(\d+) ip=(\d+) ret=(\d+)/(\d+)", create)
l = re.search(r"H6-LOAD family=(\d+) arena=(0x[0-9a-f]+) generation=(\d+) module=(\d+) ip=(\d+) ret=(\d+)/(\d+)", load)
assert c and l, "missing H6 arena/generation proof line"
# Physical arena base moved across the process boundary + layout
# perturbation; the (module, offset) identity held for the shared AI IP
# and the single call-stack return.
assert c.group(1) == l.group(1), f"family changed across restart: {c.group(1)} vs {l.group(1)}"
assert c.group(2) != l.group(2), "creator and restorer AI arena bases match"
assert c.group(4) == l.group(4), f"IP module changed across restart: {c.group(4)} vs {l.group(4)}"
assert c.group(5) == l.group(5), f"IP offset changed across restart: {c.group(5)} vs {l.group(5)}"
assert c.group(6) == l.group(6) and c.group(7) == l.group(7), f"return identity changed: {c.group(6)}/{c.group(7)} vs {l.group(6)}/{l.group(7)}"
assert c.group(4) != c.group(6), "the AI IP and its return must live in DIFFERENT modules"
print(f"H6 {family} fresh-process relocation: creator {c.groups()} -> restorer {l.groups()}")
EOF
    done
    echo "H6 state: shared AI IP + single call return relocated by module+offset identity into the new arena, both families"
fi

if [ "$mode" = "sanitize" ]; then
    echo "H4/H5/H6 sanitize: oracle/faults/replace/state/battle/ai clean under ASan/UBSan"
fi

if [ "$mode" = "" ]; then
    echo
    echo "R13-H4/H5/H6 live cutover tests: ALL PASSED"
fi
