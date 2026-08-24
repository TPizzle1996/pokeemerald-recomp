#!/usr/bin/env bash
# R13-I §20: consolidated cross-process State-v5 master harness.
#
# Proves in ONE invocation that every live pointer family restores across a
# fresh process at deliberately different arena bases (ASLR + candidate-first
# re-staging + the perturbed H layouts), plus deterministic semantic
# serialization. Each underlying runner launches its own creator/restorer
# process pair; this script is the single gate for the whole surface:
#
#   G nested field-script returns (2 frames)          run_emerald_resource_state.sh g4-cross
#   H battle blocking command + nested returns        run_emerald_resource_state.sh h3-cross
#   H live battle VM (nested, 249-opcode)             run_emerald_battle_live.sh sanitize  (state legs)
#   H animation IP + return                           (inside battle_live state legs)
#   H battle-AI + contest-AI quiescent policy         (inside battle_live state legs)
#   Dynamic RAM-script / vaddress identity            run_emerald_resource_state.sh g4-cross
#   Engine callback restoration                       run_emerald_resource_state.sh h3-cross
#   Mixed-family G+H+engine single state              run_emerald_resource_state.sh mixed-cross
#   Deterministic semantic serialization              run_emerald_resource_state.sh determinism
#
# Usage: run_emerald_state_master.sh
# Exits 0 only when every leg passes.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail=0

run() {
    echo
    echo "=== R13-I master leg: $1 ==="
    "$here/$2" > "/tmp/r13i-master-$3.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "FAIL: $1"
        tail -20 "/tmp/r13i-master-$3.log"
        fail=1
    else
        echo "ok: $1"
    fi
}

run "G fresh-process (nested returns, RAM, trainer, vaddress 8/8)" \
    run_emerald_resource_state.sh g4-cross
run "H battle fresh-process (blocking command, nested returns)" \
    run_emerald_resource_state.sh h3-cross
run "H live suite (battle + anim + AI state legs, ASan/UBSan)" \
    run_emerald_battle_live.sh sanitize
run "Mixed-family fresh-process (G+H+engine, 24 records)" \
    run_emerald_resource_state.sh mixed-cross
run "Deterministic semantic serialization (arena-base independent)" \
    run_emerald_resource_state.sh determinism

if [ "$fail" -ne 0 ]; then
    echo "R13-I master harness FAILED"
    exit 1
fi
echo "R13-I master harness: all cross-process legs green"
exit 0
