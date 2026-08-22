# R13-G6 §2 — Exact G compiled inventory (working census)

**Status:** COMPLETE 2026-08-21 (verified against build/linux64 objects + the
generated table + the qualified-ELF symbol table).
**G5 freeze baseline:** G5 report `docs/R13G5_FIELD_SCRIPT_LIVE_CUTOVER_REPORT.md`
§10 — release 25,007,320 B, DINFO 38,375,496 B, pack 20,988 entries
byte-identical, runtime loader 65,745, focused 7,143, faults 21/21. Working
tree is unchanged since G5 (nothing rebuilt in this census pass).

## 1. event_scripts.o undefined references (4,370 total)

| Class | Count | G6 disposition |
|---|---|---|
| Engine prefix (gScriptCmdTable, gScriptCmdTableEnd, gSpecials, gSpecialVars, gStdScripts, gStdScripts_End) | 6 | RETAIN (engine-owned) |
| Movement bridges (kEmeraldScriptNativeBridgeSymbols externs, generated table) | 3 | REPOINT → `bridge->bytes` (embedded 12 B from qualified ROM) |
| Text class (defined in data/text/*.inc — C-owned) | 1,533 | RETAIN compiled (R13-C) |
| map_events.o (EventScript_* inside compiled MapEvents_* arrays) | 2,260 | REMOVE (map_events.s excluded natively; the R13-F seam builds MapEvents bundles in-arena) |
| maps.o (*_MapScripts inside per-map header structs) | 469 | REMOVE (headers.inc excluded natively; gMapHeaders is HOST_DATA in map_data_native.c) |
| src/*.o C sites (108 distinct labels) | 109 | CONVERT to seam name resolution (`G_SCRIPT`) |
| **Total** | **4,370** | |

Verified per-class with nm joins (`$1=="U"` on these objects; note the
objects' nm output has no address column). C-site label → export-name
membership verified against kScriptExports with CORRECT 0-based pool
indexing (pool[0] = ""; a shifted parse misreported 19 labels as missing —
none are). The only non-export C-site labels are the 3 movement bridges.

## 2. mystery_gift.o — zero real consumers

All 6 "references" are asm register constants (REG_BASE/PLTT/OAM/etc.).
`nm` shows no undefined symbol references from any src object. The 8 gift
modules remain pack-loaded arena modules (G-owned, 692 B); only the compiled
`.s` payload leaves the native link. **Disposition: exclude natively.**

## 3. map_events.o — zero native consumers

No src object references any compiled `MapEvents_*` array symbol. The
published gMapHeaders (host array) points at seam-built in-arena bundles.
**Disposition: exclude natively.**

## 4. maps.o — retain layouts/groups, exclude headers/connections

Native maps.o defines: 36 global R (gMapGroups/gMapGroup_*, gMapLayouts) +
518 local per-map header structs + ~130 local layout/connection structs.
The per-map header structs are a DEAD LOCAL WEB natively (nothing external
references them; gMapHeaders is the host array) — their only external links
are the 469 `*_MapScripts` (and 130 `*_Layout` refs). connections.inc emits
only local symbols with zero external consumers.
**Disposition: maps.s keeps layouts.inc/layouts_table.inc/groups.inc;
headers.inc + connections.inc excluded natively (dead compiled shadows; the
F metadata stays live via the host array + pack).**

## 5. gStdScripts compiled table

`data/event_scripts.s` lines 100-112: 11 `host_pointer_entry Std_*` slots +
`gStdScripts::` / `gStdScripts_End::` labels. The Std_* symbols are G-owned
(defined in the 48 data/scripts/*.inc). The seam's `PublishStdScripts` is
the only writer.
**Disposition: natively emit 11 zeroed `.quad 0` slots under the
(NATIVE_LINUX && LINUX64) guard; GBA keeps the host_pointer_entry block.**

## 6. C-site references (108 labels / 109 refs) — conversion list

All 108 labels are kScriptExports names except the 3 movement bridges.
Per-object refs (verified): field_control_avatar 48, overworld 20,
battle_setup 8, safari_zone 4, decoration 4, fldeff_misc 3, secret_base 4,
item_use 3, fldeff_cut 2, battle_tower 2, player_pc 2,
event_object_movement 2, battle_pyramid 2, script_native_table.generated 3
(bridges), wild_encounter 1, trainer_hill 1, start_menu 2, new_game 1,
fldeff_sweetscent/strength/rocksmash/flash 1 each, berry 1, battle_main 2,
mauville_old_man 0 (mauville_old_man refs were text-class).

**Conversion design:** generated name index over ALL kScriptExports
(name → export row), seam `EmeraldScriptCompat_GetScriptSymbol(name)`
(O(log n) bsearch, resolves ModuleSpanBase + payloadOffset, no cache — no
invalidation problem), macro `G_SCRIPT(name)` in a dual-build header:
NATIVE_LINUX → `EmeraldScriptCompat_GetScriptSymbol(#name)`, else `(name)`.
The `extern const u8 EventScript_X[];` declarations stay (GBA-only usage;
dead on native).

## 7. Bridge re-pointing

The 3 bridge rows already embed the 12 qualified-ROM bytes
(`kScriptBridges[i].bytes`). The generator's
`kEmeraldScriptNativeBridgeSymbols[3]` (externs into
Route103/scripts.inc + SouthernIsland_Exterior/scripts.inc) is the ONLY
compiled-symbol reference that must die. Pack does NOT contain the bridge
keys (verified: plaintext key scan of the rpack; 234 `emerald:script/*`
keys, zero `movement/bridge`). Seam case resolves
`liveAddress = (uintptr_t)bridge->bytes`.

## 8. Ownership flip

`resources/extraction/emerald/bpee01/script/modules/ownership.generated.toml`:
523 records, all `COMPILED_PENDING_MIGRATION` → `ROM_BASE_ONLY` (native /
ownership_state), gba stays `COMPILED`. No G resource may remain
COMPILED_PENDING_MIGRATION after the flip (sweep in §12).

## 9. What stays compiled (named exclusions)

Engine prefix (cmd table, specials, gSpecialVars, gStdScripts label+zero
slots), the 35 data/text/*.inc (C-owned), gMapGroups/gMapLayouts +
layouts/groups inc (F-owned), the 3 movement bridges' embedded bytes
(B-owned, movement-isolation exception per plan §7.4), mystery-gift
dynamic interpreter + downloaded buffers (§7 MEVENT boundary), gMapHeaders
host array + map seam (F), script_cmd_table.inc.
