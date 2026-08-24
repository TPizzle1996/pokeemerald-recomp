#!/usr/bin/env python3
"""R13-H4/H5/H6: production live table (battle + battle-anim + AI +
contest-AI + field-effect).

Consumes the R13-H2/H3 generated sidecars
(resources/extraction/emerald/bpee01/battle/modules/) and emits the C
compilation table for the production live seam:

  include/emerald/resources/battle_live.generated.h
  src/emerald/resources/battle_live_table.generated.c

H4 carried the FIRST H-family live cutover (anim + field-effect only);
H5 adds the battle-script family; H6 adds the battle-AI and contest-AI
families (brief R13-H6 sec 2: module index, reloc source index, target
index, instruction boundaries, reverse containment, generation
identity, entry/routing publication). The table carries every fact the
live seam needs for the five live families:

  - the 2,149 live payload modules (640 battle + 655 anim + 553 battle-
    AI + 165 contest-AI + 68 FE: id, derived key, schema 47/48/49/50/
    51, family, arena, GBA span, canonical digest, both physical-layout
    arena offsets, boundary/export/reloc indexes) and the 8 zero-width
    alias identities (5 battle + 3 anim) with canonical owners;
  - the 5 live arenas (battle 14,413 B hull with the nested FE gap,
    battle_anim 63,811 B, battle_ai 9,303 B, contest_ai 2,524 B,
    field_effect 817 B) with both layouts (0 = GBA-preserving,
    1 = tight-packed reversed -- the semantic-identity perturbation
    proof);
  - all 7,549 reloc rows (battle 1,562: 993 SCRIPT_TARGET + 488 EWRAM +
    81 TABLE; anim 4,231; battle-AI 1,222 SCRIPT_TARGET incl. 38 data-
    target rows (if_in_* byte/hword list tables, target_offset 0, no
    instruction boundaries); contest-AI 364 SCRIPT_TARGET; FE 170),
    each with its expected runtime word (the 4 canonical .bin bytes at
    the operand offset -- the H2 oracle re-proof: ROM word ==
    final_gba == expected word), class dispatch (SCRIPT_TARGET ->
    target module + offset; ENGINE_EWRAM_TARGET -> semantic base +
    validated addend; other ENGINE_* -> semantic binding row), and
    per-module first/count indexes;
  - the 718 unique semantic binding rows (A EWRAM 50 + B table 50 +
    C sprite-template 327 + D anim-callback 213 + E gfx 11 + F FE-
    callnative 67) with native symbol addresses, addends and validated
    offsets; the 4 bindings whose native symbols do not exist in this
    fork (upstream battle_anim_mist.c / battle_anim_terrain.c are
    absent) are emitted refuse-only (address 0, no extern) and the
    resolver hard-refuses them;
  - the sorted unique SCRIPT_TARGET word index for the metadata gate on
    entry resolution (brief sec 8: raw operand matches H2 metadata,
    expected class SCRIPT_TARGET, valid target boundary, correct
    family, no compiled fallback);
  - the 7 routing tables (battle: move-effects / ball-throw /
    using-item / running-by-item / safari-actions; battle-AI:
    gBattleAI_ScriptsTable 32 rows; contest-AI: gContestAI_ScriptsTable
    32 rows) as launchable-row indexes (arena-owned canonical rows; the
    compiled tables are dead at runtime after H5/H6);
  - the compiled-label map: every battle root export + alias label with
    its compiled native symbol and its canonical GBA word, so the VM's
    direct C label references (BattleScript_Get) resolve semantically
    into the live arena;
  - the VM grammar tables (battle 300 encodings / 249 opcodes, battle-
    AI 114 / 99, contest-AI 140 / 136) with family tags for the
    differential walker (control-flow resolution at consumption).

Regeneration must be a no-op diff; run with --check.
"""

import argparse
import hashlib
import pathlib
import re
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
NATIVE_OUT = ROOT / "src" / "emerald" / "resources" \
    / "battle_live_native.generated.c"

# ---- R13-H4/H5/H6 qualified pins (docs/R13H_BATTLE_SCRIPT_MIGRATION_PLAN.md) ----
LIVE_FAMILIES = ["battle-script", "battle-anim-script", "battle-ai",
                 "contest-ai", "field-effect-script"]
# Arena order within the combined live buffer (mirrors h3 ARENA_ORDER_0/1
# restricted to the live arenas).
LIVE_ARENAS_ORDER_0 = ["battle", "battle_anim", "battle_ai", "contest_ai",
                       "field_effect"]
LIVE_ARENAS_ORDER_1 = list(reversed(LIVE_ARENAS_ORDER_0))
FAMILY_PINS = {
    "battle-script": dict(modules=645, payload=640, bytes=13592,
                          relocs=1562, schema=47, aliases=5),
    "battle-anim-script": dict(modules=658, payload=655, bytes=63811,
                               relocs=4231, schema=48, aliases=3),
    "battle-ai": dict(modules=553, payload=553, bytes=9303,
                      relocs=1222, schema=49, aliases=0),
    "contest-ai": dict(modules=165, payload=165, bytes=2524,
                       relocs=364, schema=50, aliases=0),
    "field-effect-script": dict(modules=68, payload=68, bytes=817,
                                relocs=170, schema=51, aliases=0),
}
RELOCS_PIN = 7549
BINDINGS_PIN = 718
REFUSE_ONLY_PIN = 4
# H4: 715 (anim 648 + FE 67). H5 adds the distinct battle SCRIPT_TARGET
# words (993 occurrences -> 457 distinct roots); H6 adds the distinct AI
# words (1,586 occurrences -> 708 distinct roots; no overlap with the
# battle range). Computed on first run, then pinned.
SCRIPT_TARGET_WORD_COUNT_PIN = 715 + 457 + 708
# VM grammar pins: (encodings, distinct opcodes) per family (battle 249
# opcode slots; battle-AI 99; contest-AI 136).
GRAMMAR_PINS = {
    "battle-script": (300, 249),
    "battle-ai": (114, 99),
    "contest-ai": (140, 136),
}
GRAMMAR_FILES = {
    "battle-script": "h1_grammar_battle.generated.toml",
    "battle-ai": "h1_grammar_battle_ai.generated.toml",
    "contest-ai": "h1_grammar_contest_ai.generated.toml",
}
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
# templates used by move_ice_punch's crystal table (8 occurrences). The
# other 48 B rows are battle engine-table bindings (H5).
B_LETTER_NAMES_PIN = {"gIceCrystalSpiralInwardLarge",
                      "gIceCrystalSpiralInwardSmall"}
B_LETTER_BATTLE_COUNT_PIN = 48

CLASS_LETTER = {"ENGINE_EWRAM_TARGET": "A",
                "ENGINE_SPRITE_TEMPLATE_TARGET": "C",
                "ENGINE_GFX_TARGET": "E",
                "ENGINE_TABLE_TARGET": "B"}
CLASS_ENUM = {"SCRIPT_TARGET": 0,
              "ENGINE_SPRITE_TEMPLATE_TARGET": 1,
              "ENGINE_CALLBACK": 2,
              "ENGINE_GFX_TARGET": 3,
              "ENGINE_TABLE_TARGET": 4,
              "ENGINE_EWRAM_TARGET": 5}
BINDING_LETTERS = "ABCDEF"

# Battle/AI routing tables (H1 sec 6, R13-H6 sec 2/11): module key ->
# C constant suffix. The live seam reads these rows from the arena; the
# compiled tables are dead at runtime after H5/H6. The AI tables are the
# entry points: gBattleAI_ScriptsTable[aiLogicId] (32 rows) and
# gContestAI_ScriptsTable[currentAIFlag] (32 rows).
ROUTING_TABLES = {
    "emerald:battle-script/g-battle-scripts-for-move-effects":
        "MoveEffects",
    "emerald:battle-script/g-battlescripts-for-ball-throw": "BallThrow",
    "emerald:battle-script/g-battlescripts-for-using-item": "UsingItem",
    "emerald:battle-script/g-battlescripts-for-running-by-item":
        "RunningByItem",
    "emerald:battle-script/g-battlescripts-for-safari-actions":
        "SafariActions",
    "emerald:battle-ai/g-battle-ai_scripts-table": "BattleAI",
    "emerald:contest-ai/g-contest-ai_scripts-table": "ContestAI",
}


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
    exports_name_by_key = {}
    for e in exports_toml["exports"]:
        key = e["module_key"]
        if key in exports:
            exports[key].append((e["payload_offset"],
                                 h3.EXPORT_KIND_ENUM[e["boundary_kind"]]))
            exports_name_by_key.setdefault(key, {})[
                e["payload_offset"]] = e["name"]
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

    # Battle/AI routing tables (H1 sec 6, R13-H6 sec 11): the canonical
    # GBA word of each routing module (the C runtime calls the seam with
    # this word + the row index; the seam reads the canonical row from
    # the arena). The module key embeds the family, so the routing
    # module must carry exactly that family.
    routing_words = {}
    for key in ROUTING_TABLES:
        p = payload_by_id.get(key)
        if p is None:
            fail(f"routing module {key} is not a live payload module")
        if p["family"] != family_of(key):
            fail(f"routing module {key} has family {p['family']}, "
                 f"expected {family_of(key)}")
        routing_words[key] = p["gba_start"]
    for p in payload:
        a = arenas_by_name[p["arena"]]
        p["layout_off"] = [p["gba_start"] - a["gba_start"],
                           module_off1[p["id"]]]
    for alias in aliases:
        alias["layout_off"] = [0, 0]
    arena_idx = {name: i for i, name in enumerate(LIVE_ARENAS_ORDER_0)}

    # Arena tiling: the live arena GBA spans must be exactly the union of
    # their module spans (no gaps, no overlap) - the reverse-containment
    # correctness precondition for entry-word resolution. The battle
    # hull nests the field-effect arena and declares two alignment
    # holes; every gap outside a module span must be fully covered by a
    # declared hole or a foreign arena hull clipped to this arena.
    for a in LIVE_ARENAS_ORDER_0:
        spans = sorted((p["gba_start"], p["gba_start"] + p["byte_count"])
                       for p in payload if p["arena"] == a)
        start0 = arenas_by_name[a]["gba_start"]
        end0 = arenas_by_name[a]["gba_end"]
        allowed = list(arenas_by_name[a]["holes"])
        for other in LIVE_ARENAS_ORDER_0:
            if other == a:
                continue
            oh = arenas_by_name[other]
            lo = max(oh["gba_start"], start0)
            hi = min(oh["gba_end"], end0)
            if lo < hi:
                allowed.append((lo, hi - lo))
        cursor = start0
        for start, end in spans:
            if start < cursor:
                fail(f"arena {a}: overlap at {start:#x} (cursor {cursor:#x})")
            while cursor < start:
                hit = [h for h in allowed if h[0] == cursor]
                if len(hit) != 1:
                    fail(f"arena {a}: gap at {cursor:#x} (next module "
                         f"{start:#x})")
                cursor += hit[0][1]
            if cursor != start:
                fail(f"arena {a}: gap {cursor:#x}..{start:#x} not exactly "
                     "a hole / foreign hull")
            cursor = end
        while cursor < end0:
            hit = [h for h in allowed if h[0] == cursor]
            if len(hit) != 1:
                fail(f"arena {a}: trailing gap at {cursor:#x}")
            cursor += hit[0][1]
        if cursor != end0:
            fail(f"arena {a}: tiling ends {cursor:#x}, hull ends {end0:#x}")

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
        # The RUNTIME lookup key is (letter, stored word). A (EWRAM)
        # rows encode base + addend as the stored word; every other
        # letter stores the base value itself.
        word = b["encoded_gba_value"] + b["addend"]
        key = (b["table"], word)
        if key in bindings_by_key:
            fail(f"duplicate semantic binding ({key[0]}, {key[1]:#x})")
        bindings_by_key[key] = b
        b["_word"] = word

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
            # Data targets (battle-AI if_in_* byte/hword list tables, 38
            # rows) have NO instruction boundaries and NO exports: the
            # seam accepts them at offset 0 by map kind (R13-H6 sec 6).
            # Bytecode targets must be root exports (interior targets
            # stay impossible).
            if tmod["map_kind"] == h3.MAP_KIND_ENUM["data"]:
                if r["target_offset"] != 0:
                    fail(f"{key}: data target {tkey} offset "
                         f"{r['target_offset']} != 0")
            elif r["target_offset"] not in [e[0] for e in exports[tkey]]:
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
            bkey = (letter, b["gba_base_symbol"], b["_word"])
            if bkey not in binding_usage:
                binding_usage[bkey] = dict(
                    letter=letter, name=b["gba_base_symbol"],
                    word=b["_word"],
                    native=b["native_binding_symbol"],
                    base_word=b["encoded_gba_value"],
                    addend=b["addend"],
                    allowed=b["allowed_offset"])
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
    # A (EWRAM) rows legitimately repeat the base symbol name (one row
    # per addend); every name that gets an inline extern (C/D/E/F and
    # the 2 anim B rows) must stay unique.
    inline = [b["name"] for b in bindings
              if not (b["letter"] == "A"
                      or (b["letter"] == "B"
                          and b["name"] not in B_LETTER_NAMES_PIN))]
    if len(set(inline)) != len(inline):
        fail("inline-extern binding names are not unique")
    for b in bindings:
        b["refuse_only"] = b["name"] in REFUSE_ONLY_NAMES
    binding_idx = {(b["letter"], b["word"]): i
                   for i, b in enumerate(bindings)}
    b_letter = [b for b in bindings if b["letter"] == "B"]
    b_names = {b["name"] for b in b_letter}
    if not B_LETTER_NAMES_PIN <= b_names:
        fail(f"B-letter bindings missing anim pin {B_LETTER_NAMES_PIN - b_names}")
    b_battle = [b for b in b_letter if b["name"] not in B_LETTER_NAMES_PIN]
    if len(b_battle) != B_LETTER_BATTLE_COUNT_PIN:
        fail(f"battle B-letter bindings {len(b_battle)} != pin "
             f"{B_LETTER_BATTLE_COUNT_PIN}")
    for r in relocs:
        if r["reloc_class"] != 0:
            r["binding"] = binding_idx[(r["letter"], r["expected_word"])]
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

    # Compiled-label map: every battle root export + alias label with its
    # canonical GBA word. The production VM's direct C label references
    # (BattleScript_Get) resolve through this map; the compiled label
    # symbol's native address is the lookup key (filled at link time in
    # the native-address TU).
    labels = []
    for key, rows in exports.items():
        if family_of(key) != "battle-script" or key not in payload_by_id:
            continue
        for off, _kind in rows:
            labels.append(dict(name=exports_name_by_key[key][off],
                               word=payload_by_id[key]["gba_start"] + off,
                               module=payload_idx[key], offset=off))
    seen_names = set()
    labels = [r for r in labels if not (r["name"] in seen_names
                                        or seen_names.add(r["name"]))]
    for alias in aliases:
        if alias["family"] != "battle-script":
            continue
        owner = payload_by_id[alias["owner_id"]]
        name = exports_name_by_key[alias["id"]][0]
        if name in seen_names:
            continue
        seen_names.add(name)
        labels.append(dict(name=name,
                           word=alias["gba_start"],
                           module=alias["owner_module_idx"],
                           offset=alias["gba_start"] - owner["gba_start"]))
    labels.sort(key=lambda r: r["name"])

    # The native-address TU must only reference labels that are GLOBAL in
    # the native link (the preproc marks a .s label .global only when
    # another TU references it). The runtime needs translation exactly
    # for the labels the C sources reference, so the map is exactly
    # (C-referenced BattleScript_* tokens) ∩ (battle exports); every
    # C token must land in the map (complete coverage proof).
    c_refs = set()
    for path in sorted((ROOT / "src").glob("*.c")):
        for m in re.finditer(r"BattleScript_[A-Za-z0-9_]+", path.read_text()):
            c_refs.add(m.group(0))
    labels_by_name = {r["name"]: r for r in labels}
    unmapped = sorted(c_refs - set(labels_by_name))
    if unmapped:
        fail(f"C-referenced labels missing from the battle export set: "
             f"{unmapped[:6]}...")
    labels = [labels_by_name[n] for n in sorted(c_refs)]

    # VM grammar (H1 sec 2, R13-H6 sec 5/13): battle 249 opcode slots /
    # 300 qualified encodings, battle-AI 99 / 114, contest-AI 136 / 140
    # (opcode, size, operand widths) - the differential walker's decode
    # table. Rows carry the FAMILY tag so the H6 walker selects the
    # right VM grammar at consumption (battle-AI and battle share some
    # opcode values; control-flow resolution must never cross).
    grammar = []
    for fam in ("battle-script", "battle-ai", "contest-ai"):
        grammar_toml = tomllib.loads(
            (MODULES_DIR / ".." / GRAMMAR_FILES[fam]).read_text())
        for o in grammar_toml["opcodes"]:
            widths = [0] * 5  # battle-AI if_ability/if_type: 5 operands
            for i, op in enumerate(o["operands"][:5]):
                widths[i] = op["width"]
            grammar.append(dict(opcode=o["opcode"], size=o["size"],
                                operand_count=len(o["operands"]),
                                widths=widths,
                                family_idx=fam_idx[fam]))
    grammar.sort(key=lambda r: (r["family_idx"], r["opcode"], r["size"]))
    # Same (opcode, size) encodings differ only in operand MEANING
    # (e.g. playanimation vs playanimation_var); the walker accepts any
    # matching entry, so duplicates are legal here.
    for fam, (enc, opc) in GRAMMAR_PINS.items():
        rows = [r for r in grammar if r["family_idx"] == fam_idx[fam]]
        if len(rows) != enc or len({r["opcode"] for r in rows}) != opc:
            fail(f"grammar {fam}: {len(rows)} encodings / "
                 f"{len({r['opcode'] for r in rows})} opcodes != pins "
                 f"({enc}, {opc})")

    # ---- header ----
    h = []
    h.append("/* Generated by tools/gen3_resources/battle_family/"
             "h4_generate.py.")
    h.append(" * Do not edit by hand; re-run the generator (regeneration must")
    h.append(" * be a no-op diff).")
    h.append(" *")
    h.append(" * R13-H4/H5/H6 production live table: battle + battle-anim")
    h.append(" * + battle-AI + contest-AI + field-effect arenas, relocs and")
    h.append(" * semantic bindings. Family/arena enum values come from")
    h.append(" * battle_native.generated.h so the state adapter's surface")
    h.append(" * identities line up unchanged.")
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
    h.append("#define EMERALD_BATTLE_LIVE_ROUTING_COUNT %du"
             % len(ROUTING_TABLES))
    h.append("#define EMERALD_BATTLE_LIVE_LABEL_COUNT %du" % len(labels))
    h.append("#define EMERALD_BATTLE_LIVE_GRAMMAR_ENTRY_COUNT %du"
             % len(grammar))
    h.append("")
    h.append("struct EmeraldBattleLiveGrammarEntry")
    h.append("{")
    h.append("    uint8_t opcode;")
    h.append("    uint8_t size;")
    h.append("    uint8_t operandCount;")
    h.append("    uint8_t family; /* EmeraldBattleNativeFamily; the walker")
    h.append("                       selects the VM grammar per family */")
    h.append("    uint8_t widths[5];")
    h.append("};")
    h.append("")
    h.append("/* Battle/AI routing tables (H1 sec 6, R13-H6 sec 11): the")
    h.append(" * canonical GBA address of each pointer-bearing routing module.")
    h.append(" * The live seam reads the arena-owned rows (incl. the 32-row")
    h.append(" * gBattleAI_ScriptsTable and gContestAI_ScriptsTable entry")
    h.append(" * tables); the compiled tables are dead at runtime. The enum")
    h.append(" * VALUES are the canonical GBA words - the C sites pass them")
    h.append(" * straight to ResolveRoutingTarget as the table word. */")
    h.append("enum EmeraldBattleLiveRouting")
    h.append("{")
    for key, suffix in ROUTING_TABLES.items():
        word = routing_words[key]
        h.append("    EMERALD_BATTLE_ROUTING_%s = 0x%08xu, /* %s */"
                 % (suffix.upper(), word, key))
    h.append("};")
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
    h.append("    uint8_t letter; /* 'A'..'F' */")
    h.append("    uint32_t word; /* stored word (base + addend for 'A') */")
    h.append("    uintptr_t address; /* native host address; 0 = refuse-only */")
    h.append("    uint32_t baseWord; /* 'A': encoded base value; else = word */")
    h.append("    uint32_t addend; /* 'A': validated field offset */")
    h.append("    uint32_t allowedOffset; /* 'A': ELF size bound */")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleLiveLabel")
    h.append("{")
    h.append("    const char *name;")
    h.append("    uint32_t word; /* canonical GBA address of the root */")
    h.append("    uint32_t module; /* payload module index */")
    h.append("    uint32_t offset; /* payload offset of the export */")
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
    h.append("    uint32_t labelCount;")
    h.append("    const struct EmeraldBattleLiveArena *arenas;")
    h.append("    const struct EmeraldBattleLiveModule *modules;")
    h.append("    const struct EmeraldBattleNativeBoundary *boundaries;")
    h.append("    const struct EmeraldBattleNativeExport *exports;")
    h.append("    const struct EmeraldBattleLiveAlias *aliases;")
    h.append("    const struct EmeraldBattleLiveReloc *relocs;")
    h.append("    const struct EmeraldBattleLiveBinding *bindings;")
    h.append("    const uint32_t *scriptTargetWords;")
    h.append("    const struct EmeraldBattleLiveLabel *labels;")
    h.append("    const struct EmeraldBattleLiveGrammarEntry *grammar;")
    h.append("};")
    h.append("")
    h.append("extern const struct EmeraldBattleLiveTable "
             "kEmeraldBattleLiveTable;")
    h.append("")
    h.append("/* Native-address accessors (battle_live_native.generated.c;")
    h.append(" * production-linked). Return 0 for out-of-range / non-native")
    h.append(" * rows. */")
    h.append("uintptr_t EmeraldBattleLiveNative_BindingAddress("
             "uint32_t index);")
    h.append("uintptr_t EmeraldBattleLiveNative_LabelAddress("
             "uint32_t index);")
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
        # A (EWRAM) and battle B rows resolve through the native-address
        # TU; only the inline-address letters need externs here.
        if b["letter"] == "A" or (b["letter"] == "B"
                                  and b["name"] not in B_LETTER_NAMES_PIN):
            continue
        if b["letter"] in ("B", "C"):
            c.append("extern const struct SpriteTemplate %s;" % name)
        elif b["letter"] == "E":
            c.append("extern const uint8_t %s[];" % name)
        else:
            c.append("extern void %s(void);" % name)
    c.append("")
    c.append("static const struct EmeraldBattleLiveBinding sBindings[] = {")
    for b in bindings:
        # A (EWRAM) and battle B (string-ID table) rows resolve through
        # the native-address TU (battle_live_native.generated.c); the
        # table stays platform-neutral. Everything else carries its
        # link-time address inline.
        if b["refuse_only"]:
            c.append("    /* refuse-only: %s */" % b["name"])
            c.append("    {\"%s\", '%s', 0x%08xu, 0u, 0x%08xu, %du, %du},"
                     % (b["name"], b["letter"], b["word"],
                        b["word"], b["addend"], b["allowed"]))
        elif b["letter"] == "A" or (b["letter"] == "B"
                                    and b["name"] not in B_LETTER_NAMES_PIN):
            c.append("    /* native-address TU: %s */" % b["name"])
            c.append("    {\"%s\", '%s', 0x%08xu, 0u, 0x%08xu, %du, %du},"
                     % (b["name"], b["letter"], b["word"],
                        b["base_word"], b["addend"], b["allowed"]))
        else:
            c.append("    {\"%s\", '%s', 0x%08xu, (uintptr_t)&%s, 0x%08xu,"
                     " %du, %du},"
                     % (b["name"], b["letter"], b["word"], b["name"],
                        b["word"], b["addend"], b["allowed"]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleLiveArena sArenas[] = {")
    arena_family = {
        "battle": "EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT",
        "battle_anim": "EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT",
        "battle_ai": "EMERALD_BATTLE_FAMILY_BATTLE_AI",
        "contest_ai": "EMERALD_BATTLE_FAMILY_CONTEST_AI",
        "field_effect": "EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT",
    }
    arena_family_name = {
        "battle": "battle-script",
        "battle_anim": "battle-anim-script",
        "battle_ai": "battle-ai",
        "contest_ai": "contest-ai",
        "field_effect": "field-effect-script",
    }
    for a in LIVE_ARENAS_ORDER_0:
        fam = arena_family[a]
        schema = FAMILY_PINS[arena_family_name[a]]["schema"]
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
    c.append("static const struct EmeraldBattleLiveGrammarEntry sGrammar[] = {")
    for r in grammar:
        c.append("    {%du, %du, %du, %du,"
                 " {%du, %du, %du, %du, %du}},"
                 % (r["opcode"], r["size"], r["operand_count"],
                    r["family_idx"],
                    r["widths"][0], r["widths"][1], r["widths"][2],
                    r["widths"][3], r["widths"][4]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleLiveLabel sLabels[] = {")
    for r in labels:
        c.append('    {"%s", 0x%08xu, %du, %du},'
                 % (r["name"], r["word"], r["module"], r["offset"]))
    c.append("};")
    c.append("")
    c.append("const struct EmeraldBattleLiveTable kEmeraldBattleLiveTable = {")
    c.append("    %du, %du, %du, %du, %du, %du, %du, %du, %du, %du, %du,"
             " %du,"
             % (len(modules), len(payload), len(aliases),
                len(LIVE_ARENAS_ORDER_0), len(boundaries), len(exports_flat),
                len(relocs), len(bindings), len(refuse),
                len(script_target_words), total_bytes, len(labels)))
    c.append("    sArenas, sModules, sBoundaries, sExports, sAliases,")
    c.append("    sRelocs, sBindings, sScriptTargetWords, sLabels, sGrammar,")
    c.append("};")
    table = "\n".join(c) + "\n"

    # ---- native-address TU (production-linked; the table above stays
    # platform-neutral) ----
    n = []
    n.append("/* Generated by tools/gen3_resources/battle_family/"
             "h4_generate.py.")
    n.append(" * Do not edit by hand; re-run the generator (regeneration must")
    n.append(" * be a no-op diff).")
    n.append(" */")
    n.append("")
    n.append("/* Native-typed address table for the R13-H5 battle bindings")
    n.append(" * (A EWRAM + battle B string-ID tables) and the compiled-label")
    n.append(" * map. Every symbol below is ADDRESSED (never dereferenced) in")
    n.append(" * this TU, so the uniform scalar declarations bind the linker")
    n.append(" * symbol regardless of the defining TU's real type; the real")
    n.append(" * type stays where it is defined. Missing symbols fail the")
    n.append(" * link (build-time coverage proof). */")
    n.append("")
    n.append("#include <stdint.h>")
    n.append("#include \"emerald/resources/battle_live.generated.h\"")
    n.append("")
    native_names = sorted(
        {b["name"] for b in bindings
         if b["letter"] == "A"
         or (b["letter"] == "B" and b["name"] not in B_LETTER_NAMES_PIN)}
        | {r["name"] for r in labels})
    for name in native_names:
        n.append("extern uint8_t %s;" % name)
    n.append("")
    n.append("static const uintptr_t sNativeBindingAddresses[] = {")
    for b in bindings:
        if b["letter"] == "A" or (b["letter"] == "B"
                                  and b["name"] not in B_LETTER_NAMES_PIN):
            n.append("    (uintptr_t)&%s, /* %s */" % (b["name"], b["name"]))
        else:
            n.append("    0u,")
    n.append("};")
    n.append("")
    n.append("static const uintptr_t sLabelAddresses[] = {")
    for r in labels:
        n.append("    (uintptr_t)&%s, /* %s */" % (r["name"], r["name"]))
    n.append("};")
    n.append("")
    n.append("uintptr_t EmeraldBattleLiveNative_BindingAddress("
             "uint32_t index)")
    n.append("{")
    n.append("    if (index >= EMERALD_BATTLE_LIVE_BINDING_COUNT)")
    n.append("        return 0u;")
    n.append("    return sNativeBindingAddresses[index];")
    n.append("}")
    n.append("")
    n.append("uintptr_t EmeraldBattleLiveNative_LabelAddress(uint32_t index)")
    n.append("{")
    n.append("    if (index >= EMERALD_BATTLE_LIVE_LABEL_COUNT)")
    n.append("        return 0u;")
    n.append("    return sLabelAddresses[index];")
    n.append("}")
    native = "\n".join(n) + "\n"

    h3.write_if(HEADER_OUT, header, args.check)
    h3.write_if(TABLE_OUT, table, args.check)
    h3.write_if(NATIVE_OUT, native, args.check)
    print(f"H4 table: {len(modules)} modules ({len(payload)} payload, "
          f"{len(aliases)} aliases), {len(boundaries)} boundaries, "
          f"{len(exports_flat)} export rows, {len(relocs)} relocs, "
          f"{len(bindings)} bindings ({len(refuse)} refuse-only), "
          f"{len(script_target_words)} script-target words, "
          f"{total_bytes} B")


if __name__ == "__main__":
    main()
