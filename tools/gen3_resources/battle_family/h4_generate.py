#!/usr/bin/env python3
"""R13-H4: production live table (battle-anim + field-effect arenas).

Consumes the R13-H2/H3 generated sidecars
(resources/extraction/emerald/bpee01/battle/modules/) and emits the C
compilation table for the H4 production live seam:

  include/emerald/resources/battle_live.generated.h
  src/emerald/resources/battle_live_table.generated.c

This table carries every fact the live seam needs for the FIRST H-family
live cutover (anim + field-effect only; battle/AI/contest stay compiled
through H4):

  - the 722 live payload modules (654 anim + 68 FE: id, derived key,
    schema 48/51, family, arena, GBA span, canonical digest, both
    physical-layout arena offsets, boundary/export/reloc indexes) and
    the 4 zero-width alias identities (all anim) with canonical owners;
  - the 2 live arenas (battle_anim 63,811 B, field_effect 817 B) with
    both layouts (0 = GBA-preserving, 1 = tight-packed reversed -- the
    semantic-identity perturbation proof);
  - all 4,401 reloc rows (anim 4,231 + FE 170), each with its expected
    runtime word (the 4 canonical .bin bytes at the operand offset --
    the H2 oracle re-proof: ROM word == final_gba == expected word),
    class dispatch (SCRIPT_TARGET -> target module + offset; ENGINE_* ->
    semantic binding row), and per-module first/count indexes;
  - the 620 unique semantic binding rows (C 327 + D 213 + E 11 + F 67 +
    B 2) with native symbol addresses; the 4 bindings whose native
    symbols do not exist in this fork (upstream battle_anim_mist.c /
    battle_anim_terrain.c are absent) are emitted refuse-only (address
    0, no extern) and the resolver hard-refuses them;
  - the sorted unique SCRIPT_TARGET word index (715) for the metadata
    gate on entry resolution (brief sec 8: raw operand matches H2
    metadata, expected class SCRIPT_TARGET, valid target boundary,
    family = animation, no compiled fallback).

Regeneration must be a no-op diff; run with --check.
"""

import argparse
import hashlib
import pathlib
import struct
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import h3_generate as h3  # loader functions only; h3's main() never runs

ROOT = pathlib.Path(__file__).resolve().parents[3]
MODULES_DIR = ROOT / "resources" / "extraction" / "emerald" / "bpee01" \
    / "battle" / "modules"
HEADER_OUT = ROOT / "include" / "emerald" / "resources" \
    / "battle_live.generated.h"
TABLE_OUT = ROOT / "src" / "emerald" / "resources" \
    / "battle_live_table.generated.c"

# ---- R13-H4 qualified pins (docs/R13H_BATTLE_SCRIPT_MIGRATION_PLAN.md) ----
LIVE_FAMILIES = ["battle-anim-script", "field-effect-script"]
# Arena order within the combined live buffer (mirrors h3 ARENA_ORDER_0/1
# restricted to the two live arenas).
LIVE_ARENAS_ORDER_0 = ["battle_anim", "field_effect"]
LIVE_ARENAS_ORDER_1 = list(reversed(LIVE_ARENAS_ORDER_0))
FAMILY_PINS = {
    "battle-anim-script": dict(modules=658, payload=655, bytes=63811,
                               relocs=4231, schema=48, aliases=3),
    "field-effect-script": dict(modules=68, payload=68, bytes=817,
                                relocs=170, schema=51, aliases=0),
}
RELOCS_PIN = 4401
BINDINGS_PIN = 620
REFUSE_ONLY_PIN = 4
SCRIPT_TARGET_WORD_COUNT_PIN = 715
# Native binding symbols that do NOT exist in this fork (upstream
# src/battle_anim_mist.c + src/battle_anim_terrain.c are absent). The
# qualified ROM references them from 13 move/effect modules; the live
# seam refuses these rows (address 0), never fabricates a fallback.
REFUSE_ONLY_NAMES = {
    "gShakeMonOrTerrainSpriteTemplate",
    "AnimTask_GetBattleTerrain",
    "AnimTask_LoadMistTiles",
    "AnimTask_ShakeBattleTerrain",
}
# B-letter bindings referenced by anim relocs: the 2 gIceCrystal* sprite
# templates used by move_ice_punch's crystal table (8 occurrences).
B_LETTER_NAMES_PIN = {"gIceCrystalSpiralInwardLarge",
                      "gIceCrystalSpiralInwardSmall"}

CLASS_LETTER = {"ENGINE_SPRITE_TEMPLATE_TARGET": "C",
                "ENGINE_GFX_TARGET": "E",
                "ENGINE_TABLE_TARGET": "B"}
CLASS_ENUM = {"SCRIPT_TARGET": 0,
              "ENGINE_SPRITE_TEMPLATE_TARGET": 1,
              "ENGINE_CALLBACK": 2,
              "ENGINE_GFX_TARGET": 3,
              "ENGINE_TABLE_TARGET": 4}
BINDING_LETTERS = "BCDEF"


def fail(msg):
    print(f"H4 ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def family_of(module_key):
    return module_key[len("emerald:"):].split("/", 1)[0]


def name_of(module_key):
    return module_key.split("/", 1)[1]


def compute_live_layouts(arenas_by_name, payload):
    """Combined-buffer arena offsets + per-module arena-relative offsets for
    the two live arenas, both layouts (same geometry rules as h3)."""
    pack = {a: sorted((p for p in payload if p["arena"] == a),
                      key=lambda r: r["id"])
            for a in LIVE_ARENAS_ORDER_0}
    tight_size = {a: sum(p["byte_count"] for p in pack[a])
                  for a in LIVE_ARENAS_ORDER_0}
    off0, off1 = {}, {}
    for a in LIVE_ARENAS_ORDER_0:
        off0[a] = sum(arenas_by_name[x]["hull_bytes"]
                      for x in LIVE_ARENAS_ORDER_0
                      [:LIVE_ARENAS_ORDER_0.index(a)])
    for a in LIVE_ARENAS_ORDER_1:
        off1[a] = sum(tight_size[x]
                      for x in LIVE_ARENAS_ORDER_1
                      [:LIVE_ARENAS_ORDER_1.index(a)])
    module_off1 = {}
    for a in LIVE_ARENAS_ORDER_0:
        cursor = 0
        for p in pack[a]:
            module_off1[p["id"]] = cursor
            cursor += p["byte_count"]
    arena_layout = {}
    for a in LIVE_ARENAS_ORDER_0:
        arena_layout[a] = dict(
            layout_size=[arenas_by_name[a]["hull_bytes"], tight_size[a]],
            layout_offset=[off0[a], off1[a]])
    return arena_layout, module_off1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="verify regeneration is a no-op diff")
    args = parser.parse_args()

    arenas_by_name, _ = h3.load_arenas()
    payload_all, aliases_all = h3.load_modules(arenas_by_name)
    payload = [p for p in payload_all if p["family"] in LIVE_FAMILIES]
    aliases = [a for a in aliases_all if a["family"] in LIVE_FAMILIES]
    for fam, pin in FAMILY_PINS.items():
        mods = [p for p in payload + aliases if p["family"] == fam]
        if len(mods) != pin["modules"]:
            fail(f"family {fam} modules {len(mods)} != pin {pin['modules']}")
        pay = [p for p in payload if p["family"] == fam]
        if len(pay) != pin["payload"]:
            fail(f"family {fam} payload {len(pay)} != pin {pin['payload']}")
        byt = sum(p["byte_count"] for p in mods)
        if byt != pin["bytes"]:
            fail(f"family {fam} bytes {byt} != pin {pin['bytes']}")
        ali = [a for a in aliases if a["family"] == fam]
        if len(ali) != pin["aliases"]:
            fail(f"family {fam} aliases {len(ali)} != pin {pin['aliases']}")

    # Family/arena attribution (h3.load_modules leaves these to h3.main).
    fam_idx = {name: i for i, name in enumerate(h3.FAMILY_PINS)}
    for rec in payload + aliases:
        rec["family_idx"] = fam_idx[rec["family"]]
        rec["arena"] = h3.FAMILY_ARENA[rec["family"]]

    payload_by_id = {p["id"]: p for p in payload}
    aliases_by_id = {a["id"]: a for a in aliases}

    # Boundary maps + exports: load the sidecars and filter to live keys
    # (h3's loaders validate the full 5-family surface and reject a
    # filtered view).
    maps_toml = tomllib.loads(
        (MODULES_DIR / "boundary_maps.generated.toml").read_text())
    maps = {}
    for m in maps_toml["maps"]:
        key = m["module_key"]
        if key not in payload_by_id and key not in aliases_by_id:
            continue
        maps[key] = (h3.MAP_KIND_ENUM[m["kind"]],
                     [(b["payload_offset"], b["length"])
                      for b in m.get("boundaries", [])])
    missing = (set(payload_by_id) | set(aliases_by_id)) - set(maps)
    if missing:
        fail(f"modules missing boundary maps: {sorted(missing)[:4]}...")
    exports_toml = tomllib.loads(
        (MODULES_DIR / "exports.generated.toml").read_text())
    exports = {k: [] for k in list(payload_by_id) + list(aliases_by_id)}
    for e in exports_toml["exports"]:
        key = e["module_key"]
        if key in exports:
            exports[key].append((e["payload_offset"],
                                 h3.EXPORT_KIND_ENUM[e["boundary_kind"]]))
    for key, rows in exports.items():
        rows.sort()

    # Canonical bytes: verify every live payload digest + keep the bytes
    # for the expected-word reads.
    for p in payload:
        data = (MODULES_DIR / "data" / p["family"]
                / f"{name_of(p['id'])}.bin").read_bytes()
        if len(data) != p["byte_count"]:
            fail(f"{p['id']}: canonical .bin size {len(data)} != "
                 f"{p['byte_count']}")
        if hashlib.sha256(data).digest() != p["digest"]:
            fail(f"{p['id']}: canonical .bin digest mismatch")
        p["data"] = data

    for alias in aliases:
        owners = [p for p in payload
                  if p["gba_start"] <= alias["gba_start"]
                  < p["gba_start"] + p["byte_count"]]
        if len(owners) != 1:
            fail(f"alias {alias['id']} has {len(owners)} owners, not 1")
        alias["owner_id"] = owners[0]["id"]

    modules = payload + aliases  # payload sorted by id, then aliases
    payload_idx = {p["id"]: i for i, p in enumerate(payload)}
    for alias in aliases:
        alias["owner_module_idx"] = payload_idx[alias["owner_id"]]
    arena_layout, module_off1 = compute_live_layouts(arenas_by_name, payload)
    for p in payload:
        a = arenas_by_name[p["arena"]]
        p["layout_off"] = [p["gba_start"] - a["gba_start"],
                           module_off1[p["id"]]]
    for alias in aliases:
        alias["layout_off"] = [0, 0]
    arena_idx = {name: i for i, name in enumerate(LIVE_ARENAS_ORDER_0)}

    # Arena tiling: the live arena GBA spans must be exactly the union of
    # their module spans (no gaps, no overlap) - the reverse-containment
    # correctness precondition for entry-word resolution.
    for a in LIVE_ARENAS_ORDER_0:
        spans = sorted((p["gba_start"], p["gba_start"] + p["byte_count"])
                       for p in payload if p["arena"] == a)
        cursor = arenas_by_name[a]["gba_start"]
        for start, end in spans:
            if start != cursor:
                fail(f"arena {a}: gap at {cursor:#x} (next module {start:#x})")
            cursor = end
        if cursor != arenas_by_name[a]["gba_end"]:
            fail(f"arena {a}: tiling ends {cursor:#x}, hull ends "
                 f"{arenas_by_name[a]['gba_end']:#x}")

    # Boundary/export flat arrays (anim + FE only).
    boundaries, exports_flat = [], []
    for rec in modules:
        kind, intervals = maps[rec["id"]]
        rec["map_kind"] = kind
        rec["boundary_first"] = len(boundaries)
        boundaries.extend(intervals)
        rec["boundary_count"] = len(intervals)
        rec["export_first"] = len(exports_flat)
        exports_flat.extend(exports[rec["id"]])
        rec["export_count"] = len(exports[rec["id"]])

    # Relocs: anim + FE rows with expected runtime words (canonical .bin
    # bytes) and target/binding dispatch.
    relocs_toml = tomllib.loads(
        (MODULES_DIR / "relocations.generated.toml").read_text())
    bindings_toml = tomllib.loads(
        (MODULES_DIR / "bindings.generated.toml").read_text())
    bindings_by_key = {}
    for b in bindings_toml["semantic_tables"]:
        key = (b["table"], b["encoded_gba_value"])
        # Letter A (EWRAM) bindings legitimately share base addresses
        # (multiple fields per base); the letters H4 uses are unique.
        if key in bindings_by_key and b["table"] != "A":
            fail(f"duplicate semantic binding ({key[0]}, {key[1]:#x})")
        bindings_by_key[key] = b

    relocs = []
    word_index = set()
    binding_usage = {}
    for r in relocs_toml["relocs"]:
        key = r["module_key"]
        fam = family_of(key)
        if fam not in LIVE_FAMILIES:
            continue
        p = payload_by_id[key]
        if p is None:
            fail(f"reloc for unknown payload module {key}")
        if r["operand_width"] != 4:
            fail(f"{key}: operand width {r['operand_width']} != 4")
        word = struct.unpack_from("<I", p["data"], r["operand_offset"])[0]
        if word != r["final_gba"]:
            fail(f"{key}@{r['operand_offset']}: canonical word {word:#x} != "
                 f"final_gba {r['final_gba']:#x} (H2 oracle re-proof)")
        row = dict(module=payload_idx[key], operand_offset=r["operand_offset"],
                   expected_word=word,
                   reloc_class=CLASS_ENUM[r["target_class"]], width=4)
        if r["target_class"] == "SCRIPT_TARGET":
            tkey = r["target_resource_key"]
            tmod = payload_by_id.get(tkey)
            if tmod is None:
                fail(f"{key}: SCRIPT_TARGET target {tkey} is not a payload "
                     "module")
            if tmod["family"] != fam:
                fail(f"{key}: cross-family SCRIPT_TARGET -> {tkey}")
            if not (0 <= r["target_offset"] < tmod["byte_count"]):
                fail(f"{key}: target offset {r['target_offset']} out of "
                     f"{tkey} range")
            if r["target_offset"] not in [e[0] for e in exports[tkey]]:
                fail(f"{key}: target offset {r['target_offset']} is not a "
                     f"root export of {tkey} (interior target)")
            row["target"] = payload_idx[tkey]
            row["target_offset"] = r["target_offset"]
            word_index.add(word)
        else:
            if r["target_class"] == "ENGINE_CALLBACK":
                letter = "D" if fam == "battle-anim-script" else "F"
            else:
                letter = CLASS_LETTER[r["target_class"]]
            b = bindings_by_key.get((letter, word))
            if b is None:
                fail(f"{key}@{r['operand_offset']}: no binding row for "
                     f"({letter}, {word:#x})")
            bkey = (letter, b["gba_base_symbol"], b["encoded_gba_value"])
            if bkey not in binding_usage:
                binding_usage[bkey] = dict(letter=letter, name=b["gba_base_symbol"],
                                           word=b["encoded_gba_value"],
                                           native=b["native_binding_symbol"])
            row["letter"] = letter
        relocs.append(row)
    if len(relocs) != RELOCS_PIN:
        fail(f"relocs {len(relocs)} != pin {RELOCS_PIN}")
    for fam, pin in FAMILY_PINS.items():
        n = sum(1 for r in relocs
                if payload[r["module"]]["family"] == fam)
        if n != pin["relocs"]:
            fail(f"family {fam} relocs {n} != pin {pin['relocs']}")

    # Binding table: sorted (letter, word); refuse-only rows for the 4
    # absent native symbols (each must be referenced by >= 1 reloc row).
    bindings = sorted(binding_usage.values(),
                      key=lambda b: (BINDING_LETTERS.index(b["letter"]),
                                     b["word"]))
    if len(bindings) != BINDINGS_PIN:
        fail(f"bindings {len(bindings)} != pin {BINDINGS_PIN}")
    refuse = [b for b in bindings if b["name"] in REFUSE_ONLY_NAMES]
    if len(refuse) != REFUSE_ONLY_PIN:
        fail(f"refuse-only bindings {len(refuse)} != pin {REFUSE_ONLY_PIN}")
    extra_refuse = REFUSE_ONLY_NAMES - {b["name"] for b in refuse}
    if extra_refuse:
        fail(f"refuse-only names not referenced: {sorted(extra_refuse)}")
    names = [b["name"] for b in bindings]
    if len(set(names)) != len(names):
        fail("binding names are not unique (extern emission would collide)")
    for b in bindings:
        b["refuse_only"] = b["name"] in REFUSE_ONLY_NAMES
    binding_idx = {b["name"]: i for i, b in enumerate(bindings)}
    b_letter = [b for b in bindings if b["letter"] == "B"]
    if {b["name"] for b in b_letter} != B_LETTER_NAMES_PIN:
        fail(f"B-letter bindings {[b['name'] for b in b_letter]} != pin")
    for r in relocs:
        if r["reloc_class"] != 0:
            letter = r["letter"]
            name = next(b["name"] for b in bindings
                        if b["letter"] == letter
                        and b["word"] == r["expected_word"])
            r["binding"] = binding_idx[name]
    referenced = {r["binding"] for r in relocs if r["reloc_class"] != 0}
    if referenced != set(range(BINDINGS_PIN)):
        fail("binding rows referenced by relocs != full binding table")

    # Reloc rows sorted by (module, operandOffset); per-module indexes
    # (zero-width aliases carry no relocs and no host span).
    relocs.sort(key=lambda r: (r["module"], r["operand_offset"]))
    for p in payload:
        idx = payload_idx[p["id"]]
        p["reloc_first"] = sum(1 for r in relocs if r["module"] < idx)
        rows = [r for r in relocs if r["module"] == idx]
        p["reloc_count"] = len(rows)
        seen = set()
        for r in rows:
            if r["operand_offset"] in seen:
                fail(f"{p['id']}: duplicate reloc operand offset "
                     f"{r['operand_offset']}")
            seen.add(r["operand_offset"])
    for alias in aliases:
        alias["reloc_first"] = 0
        alias["reloc_count"] = 0
    script_target_words = sorted(word_index)
    if len(script_target_words) != SCRIPT_TARGET_WORD_COUNT_PIN:
        fail(f"script-target words {len(script_target_words)} != pin "
             f"{SCRIPT_TARGET_WORD_COUNT_PIN}")

    total_bytes = sum(p["byte_count"] for p in payload)

    # ---- header ----
    h = []
    h.append("/* Generated by tools/gen3_resources/battle_family/"
             "h4_generate.py.")
    h.append(" * Do not edit by hand; re-run the generator (regeneration must")
    h.append(" * be a no-op diff).")
    h.append(" *")
    h.append(" * R13-H4 production live table: battle-anim + field-effect")
    h.append(" * arenas, relocs and semantic bindings. Family/arena enum")
    h.append(" * values come from battle_native.generated.h so the state")
    h.append(" * adapter's surface identities line up unchanged.")
    h.append(" */")
    h.append("")
    h.append("#ifndef EMERALD_RESOURCES_BATTLE_LIVE_GENERATED_H")
    h.append("#define EMERALD_RESOURCES_BATTLE_LIVE_GENERATED_H")
    h.append("")
    h.append("#include <stdint.h>")
    h.append("#include \"emerald/resources/battle_native.generated.h\"")
    h.append("")
    h.append("#define EMERALD_BATTLE_LIVE_ARENA_COUNT %du"
             % len(LIVE_ARENAS_ORDER_0))
    h.append("#define EMERALD_BATTLE_LIVE_MODULE_COUNT %du" % len(modules))
    h.append("#define EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT %du"
             % len(payload))
    h.append("#define EMERALD_BATTLE_LIVE_ALIAS_COUNT %du" % len(aliases))
    h.append("#define EMERALD_BATTLE_LIVE_BOUNDARY_COUNT %du"
             % len(boundaries))
    h.append("#define EMERALD_BATTLE_LIVE_EXPORT_ROW_COUNT %du"
             % len(exports_flat))
    h.append("#define EMERALD_BATTLE_LIVE_RELOC_COUNT %du" % len(relocs))
    h.append("#define EMERALD_BATTLE_LIVE_BINDING_COUNT %du" % len(bindings))
    h.append("#define EMERALD_BATTLE_LIVE_REFUSE_ONLY_BINDING_COUNT %du"
             % len(refuse))
    h.append("#define EMERALD_BATTLE_LIVE_SCRIPT_TARGET_WORD_COUNT %du"
             % len(script_target_words))
    h.append("#define EMERALD_BATTLE_LIVE_CANONICAL_BYTES %du" % total_bytes)
    h.append("#define EMERALD_BATTLE_LIVE_LAYOUT_COUNT 2u")
    h.append("")
    h.append("enum EmeraldBattleLiveRelocClass")
    h.append("{")
    for name in CLASS_ENUM:
        h.append("    EMERALD_BATTLE_LIVE_RELOC_%s = %du,"
                 % (name, CLASS_ENUM[name]))
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveArena")
    h.append("{")
    h.append("    uint32_t family; /* EmeraldBattleNativeFamily */")
    h.append("    uint32_t schema;")
    h.append("    uint32_t gbaStart;")
    h.append("    uint32_t gbaEnd; /* exclusive */")
    h.append("    uint32_t hullBytes;")
    h.append("    uint32_t layoutSize[EMERALD_BATTLE_LIVE_LAYOUT_COUNT];")
    h.append("    uint32_t layoutOffset[EMERALD_BATTLE_LIVE_LAYOUT_COUNT];")
    h.append("    /* within the combined live buffer */")
    h.append("    uint32_t moduleFirst;")
    h.append("    uint32_t moduleCount;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveModule")
    h.append("{")
    h.append("    const char *id;")
    h.append("    uint8_t key[32];")
    h.append("    uint32_t schema;")
    h.append("    uint32_t family; /* EmeraldBattleNativeFamily */")
    h.append("    uint32_t arena; /* index into table.arenas */")
    h.append("    uint32_t gbaStart;")
    h.append("    uint32_t byteCount;")
    h.append("    uint8_t digest[32];")
    h.append("    uint32_t layoutOffset[EMERALD_BATTLE_LIVE_LAYOUT_COUNT];")
    h.append("    /* arena-relative */")
    h.append("    uint32_t boundaryFirst;")
    h.append("    uint32_t boundaryCount;")
    h.append("    uint32_t exportFirst;")
    h.append("    uint32_t exportCount;")
    h.append("    uint32_t relocFirst;")
    h.append("    uint32_t relocCount;")
    h.append("    uint32_t mapKind; /* EmeraldBattleNativeMapKind */")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveAlias")
    h.append("{")
    h.append("    const char *id;")
    h.append("    uint32_t family;")
    h.append("    uint32_t ownerModule; /* canonical payload owner */")
    h.append("    uint32_t gbaStart;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveBinding")
    h.append("{")
    h.append("    const char *name;")
    h.append("    uint8_t letter; /* 'B' | 'C' | 'D' | 'E' | 'F' */")
    h.append("    uint32_t word; /* encoded GBA value (canonical word) */")
    h.append("    uintptr_t address; /* native host address; 0 = refuse-only */")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveReloc")
    h.append("{")
    h.append("    uint32_t module; /* payload module index */")
    h.append("    uint32_t operandOffset;")
    h.append("    uint32_t expectedWord;")
    h.append("    uint16_t relocClass; /* EmeraldBattleLiveRelocClass */")
    h.append("    uint16_t width;")
    h.append("    uint32_t target; /* SCRIPT_TARGET: module index;")
    h.append("                       ENGINE_*: binding index */")
    h.append("    uint32_t targetOffset; /* SCRIPT_TARGET only */")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveTable")
    h.append("{")
    h.append("    uint32_t moduleCount;")
    h.append("    uint32_t payloadModuleCount;")
    h.append("    uint32_t aliasCount;")
    h.append("    uint32_t arenaCount;")
    h.append("    uint32_t boundaryCount;")
    h.append("    uint32_t exportRowCount;")
    h.append("    uint32_t relocCount;")
    h.append("    uint32_t bindingCount;")
    h.append("    uint32_t refuseOnlyBindingCount;")
    h.append("    uint32_t scriptTargetWordCount;")
    h.append("    uint32_t canonicalBytes;")
    h.append("    const struct EmeraldBattleLiveArena *arenas;")
    h.append("    const struct EmeraldBattleLiveModule *modules;")
    h.append("    const struct EmeraldBattleNativeBoundary *boundaries;")
    h.append("    const struct EmeraldBattleNativeExport *exports;")
    h.append("    const struct EmeraldBattleLiveAlias *aliases;")
    h.append("    const struct EmeraldBattleLiveReloc *relocs;")
    h.append("    const struct EmeraldBattleLiveBinding *bindings;")
    h.append("    const uint32_t *scriptTargetWords;")
    h.append("};")
    h.append("")
    h.append("extern const struct EmeraldBattleLiveTable "
             "kEmeraldBattleLiveTable;")
    h.append("")
    h.append("#endif /* EMERALD_RESOURCES_BATTLE_LIVE_GENERATED_H */")
    header = "\n".join(h) + "\n"

    # ---- C table ----
    c = []
    c.append("/* Generated by tools/gen3_resources/battle_family/"
             "h4_generate.py.")
    c.append(" * Do not edit by hand; re-run the generator (regeneration must")
    c.append(" * be a no-op diff).")
    c.append(" */")
    c.append("")
    c.append("#include \"emerald/resources/battle_live.generated.h\"")
    c.append("")
    c.append("/* struct SpriteTemplate (forward): the externs below only need")
    c.append(" * the tag - this file stays platform-neutral (no global.h, no")
    c.append(" * sprite.h); consumers that dereference bindings include the")
    c.append(" * full definition via global.h. */")
    c.append("struct SpriteTemplate;")
    c.append("")
    c.append("/* Semantic binding externs. The native symbol NAME is the")
    c.append(" * semantic identity (qualified ROM and this fork share the")
    c.append(" * pret source symbol); the address is the fork's own link-time")
    c.append(" * value - no GBA->host arithmetic anywhere. The 4 refuse-only")
    c.append(" * bindings have no extern (their upstream defining files,")
    c.append(" * battle_anim_mist.c / battle_anim_terrain.c, are absent from")
    c.append(" * this fork): the resolver hard-refuses them (address 0).")
    c.append(" */")
    for b in sorted(bindings, key=lambda b: b["name"]):
        if b["refuse_only"]:
            continue
        name = b["name"]
        if b["letter"] in ("B", "C"):
            c.append("extern const struct SpriteTemplate %s;" % name)
        elif b["letter"] == "E":
            c.append("extern const uint8_t %s[];" % name)
        else:
            c.append("extern void %s(void);" % name)
    c.append("")
    c.append("static const struct EmeraldBattleLiveBinding sBindings[] = {")
    for b in bindings:
        if b["refuse_only"]:
            c.append("    /* refuse-only: %s */" % b["name"])
            c.append("    {\"%s\", '%s', 0x%08xu, 0u},"
                     % (b["name"], b["letter"], b["word"]))
        else:
            c.append("    {\"%s\", '%s', 0x%08xu, (uintptr_t)&%s},"
                     % (b["name"], b["letter"], b["word"], b["name"]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleLiveArena sArenas[] = {")
    for a in LIVE_ARENAS_ORDER_0:
        fam = "EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT" if a == "battle_anim" \
            else "EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT"
        schema = FAMILY_PINS["battle-anim-script" if a == "battle_anim"
                            else "field-effect-script"]["schema"]
        la = arena_layout[a]
        mods = [p for p in payload if p["arena"] == a]
        first = payload_idx[mods[0]["id"]]
        c.append("    {%s, %du, 0x%x, 0x%x, %du, {%du, %du}, {%du, %du}, "
                 "%du, %du},"
                 % (fam, schema, arenas_by_name[a]["gba_start"],
                    arenas_by_name[a]["gba_end"],
                    arenas_by_name[a]["hull_bytes"],
                    la["layout_size"][0], la["layout_size"][1],
                    la["layout_offset"][0], la["layout_offset"][1],
                    first, len(mods)))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeBoundary sBoundaries[] = {")
    for off, length in boundaries:
        c.append("    {%du, %du}," % (off, length))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeExport sExports[] = {")
    for off, ekind in exports_flat:
        c.append("    {%du, %du}," % (off, ekind))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleLiveAlias sAliases[] = {")
    for rec in aliases:
        c.append('    {"%s", %du, %du, 0x%x},'
                 % (rec["id"], rec["family_idx"], rec["owner_module_idx"],
                    rec["gba_start"]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleLiveModule sModules[] = {")
    for rec in modules:
        c.append('    {"%s",' % rec["id"])
        c.append("     {%s}," % h3.c_bytes(rec["key"]))
        c.append("     %du, %du, %du, 0x%x, %du,"
                 % (rec["schema"], rec["family_idx"], arena_idx[rec["arena"]],
                    rec["gba_start"], rec["byte_count"]))
        c.append("     {%s}," % h3.c_digest(rec["digest"].hex()))
        c.append("     {%du, %du}, %du, %du, %du, %du, %du, %du, %du},"
                 % (rec["layout_off"][0], rec["layout_off"][1],
                    rec["boundary_first"], rec["boundary_count"],
                    rec["export_first"], rec["export_count"],
                    rec["reloc_first"], rec["reloc_count"],
                    rec["map_kind"]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleLiveReloc sRelocs[] = {")
    for r in relocs:
        if r["reloc_class"] == 0:
            c.append("    {%du, %du, 0x%08xu, %du, %du, %du, %du},"
                     % (r["module"], r["operand_offset"], r["expected_word"],
                        r["reloc_class"], r["width"], r["target"],
                        r["target_offset"]))
        else:
            c.append("    {%du, %du, 0x%08xu, %du, %du, %du, 0},"
                     % (r["module"], r["operand_offset"], r["expected_word"],
                        r["reloc_class"], r["width"], r["binding"]))
    c.append("};")
    c.append("")
    c.append("static const uint32_t sScriptTargetWords[] = {")
    for w in script_target_words:
        c.append("    0x%08xu," % w)
    c.append("};")
    c.append("")
    c.append("const struct EmeraldBattleLiveTable kEmeraldBattleLiveTable = {")
    c.append("    %du, %du, %du, %du, %du, %du, %du, %du, %du, %du, %du,"
             % (len(modules), len(payload), len(aliases),
                len(LIVE_ARENAS_ORDER_0), len(boundaries), len(exports_flat),
                len(relocs), len(bindings), len(refuse),
                len(script_target_words), total_bytes))
    c.append("    sArenas, sModules, sBoundaries, sExports, sAliases,")
    c.append("    sRelocs, sBindings, sScriptTargetWords,")
    c.append("};")
    table = "\n".join(c) + "\n"

    h3.write_if(HEADER_OUT, header, args.check)
    h3.write_if(TABLE_OUT, table, args.check)
    print(f"H4 table: {len(modules)} modules ({len(payload)} payload, "
          f"{len(aliases)} aliases), {len(boundaries)} boundaries, "
          f"{len(exports_flat)} export rows, {len(relocs)} relocs, "
          f"{len(bindings)} bindings ({len(refuse)} refuse-only), "
          f"{len(script_target_words)} script-target words, "
          f"{total_bytes} B")


if __name__ == "__main__":
    main()
