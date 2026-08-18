#!/bin/sh
set -eu

binary=${1:-./pokeemerald-linux64}
test -x "$binary"

symbol_address()
{
    nm -n "$binary" | awk -v symbol="$1" '$3 == symbol { print $1; exit }'
}

game_bss_start=$(symbol_address __start_game_bss)
game_bss_end=$(symbol_address __stop_game_bss)
game_data_start=$(symbol_address __start_game_data)
game_data_end=$(symbol_address __stop_game_data)
stdout_address=$(symbol_address 'stdout@GLIBC_2.2.5')
stderr_address=$(symbol_address 'stderr@GLIBC_2.2.5')
stdin_address=$(symbol_address 'stdin@GLIBC_2.2.5')

test -n "$game_bss_start" -a -n "$game_bss_end"
test -n "$game_data_start" -a -n "$game_data_end"
test -n "$stdout_address" -a -n "$stderr_address" -a -n "$stdin_address"
test "$game_bss_start" \< "$game_bss_end"
test "$game_data_start" \< "$game_data_end"
test "$game_bss_end" \< "$stdout_address"
test "$game_bss_end" \< "$stderr_address"
test "$game_bss_end" \< "$stdin_address"

copy_count=$(readelf -rW "$binary" \
    | awk '$3 == "R_X86_64_COPY" && $5 ~ /^(stdin|stdout|stderr)@GLIBC_2\.2\.5$/ { count++ } END { print count + 0 }')
test "$copy_count" -eq 3

data_root=$(mktemp -d "${TMPDIR:-/tmp}/pokeemerald-native-state.XXXXXX")
trap 'rm -rf "$data_root"' EXIT
output=$(POKEEMERALD_DATA_ROOT="$data_root" "$binary" --native-state-self-test)
printf '%s\n' "$output"
printf '%s\n' "$output" | grep -q 'Native state self-test passed (quick and slot 1)'
printf '%s\n' "$output" | grep -q 'Native state self-test passed (trainer family republish)'

test -s "$data_root/profiles/default/states/quick.state"
test -s "$data_root/profiles/default/states/slot1.state"

printf '%s\n' 'native state regression passed'
