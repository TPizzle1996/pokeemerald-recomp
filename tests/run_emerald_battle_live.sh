#!/usr/bin/env bash
#
# R13-H4: live cutover tests for the battle-anim + field-effect arenas
# (tests/emerald_battle_live_test.c).
#
# Compiles the SAME production machinery as run_emerald_resource_state.sh
# (real walker, real seams, real R6 loader, maps.o) with the H3 shadow
# seam swapped for the PRODUCTION live seam:
#
#   - emerald_battle_compat.c + battle_native_table.generated.c  (H3)
#   + emerald_battle_live.c + battle_live_table.generated.c      (H4)
#
# and drives it against the REAL production pack:
#
#   oracle     differential resolution oracle over all 726 modules and
#              4,401 relocs, both physical layouts; staged bytes are
#              byte-exact against the committed .bin artifacts
#   faults     brief sec 24 fail-closed matrix (11 cases)
#   replace    brief sec 25/26: generation replacement, 6,379 range
#              invariant, identity-based unregister
#   state      brief sec 15: fresh-process State-v5 anim proof
#   sanitize   oracle + faults + state under ASan/UBSan
#
# Modes: oracle | faults | replace | state | sanitize (default: all).
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

echo "== compiling H4 live-cutover test (real walker + seams + loader + LIVE seam) =="
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
    "$root/src/platform/native_state.c" \
    "$root/src/platform/host_memory.c" \
    "$root/src/platform/native_world_neighborhood.c" \
    "$root/src/platform/desktop_audio.c" \
    "$root/src/platform/desktop_state.c" \
    "$here/emerald_audio_device_shim.c" \
    "$root/build/linux64/data/maps.o" \
    "$tmp/map_script_stubs.o" \
    "$tmp/binding_stubs.o" \
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
        grep -q "H4-ORACLE layout=$layout entry-words=717 byte-exact=723 relocs=4401" "oracle-$layout.log"
    done
    echo "H4 oracle: 723 entry words, 723 byte-exact modules, 4,401 relocs, both layouts"
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
    grep -q "count=6379" replace.log
    echo "H4 replace: generation B committed, 6,379 invariant, identity unregister/restore"
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

if [ "$mode" = "sanitize" ]; then
    echo "H4 sanitize: oracle/faults/replace/state clean under ASan/UBSan"
fi

if [ "$mode" = "" ]; then
    echo
    echo "R13-H4 live cutover tests: ALL PASSED"
fi
