#!/usr/bin/env python3
"""R13-H3: State-v5 relocation-readiness table emission (shadow only).

Consumes the R13-H2 generated sidecars
(resources/extraction/emerald/bpee01/battle/modules/) and emits the C
compilation table for the H3 shadow seam + state adapter:

  include/emerald/resources/battle_native.generated.h
  src/emerald/resources/battle_native_table.generated.c

The table carries every fact the shadow seam needs without touching the
H2 artifacts: the 2,081 payload modules (id, derived key, schema, family,
arena, GBA span, canonical digest, per-layout arena offsets, boundary +
export indexes), the 8 zero-width alias identities with their canonical
payload owner, the five family arenas (hull + holes), and two physical
layouts:

  layout 0 "gba":  modules placed at (gba_start - arena.gba_start)
                   within each arena, arenas in sidecar order -- the
                   h2_stage.py geometry, verbatim.
  layout 1 "tight": modules packed back-to-back in module-id order
                   within each arena, arenas in REVERSED order -- a
                   deliberate physical perturbation proving that
                   persisted identity is semantic module + offset,
                   never an aggregate offset (R13-H3 brief sec 22).

The seam stages either layout per generation; both must resolve the same
semantic identity. Regeneration must be a no-op diff; run with --check.
"""

import argparse
import hashlib
import pathlib
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[3]
MODULES_DIR = ROOT / "resources" / "extraction" / "emerald" / "bpee01" \
    / "battle" / "modules"
HEADER_OUT = ROOT / "include" / "emerald" / "resources" \
    / "battle_native.generated.h"
TABLE_OUT = ROOT / "src" / "emerald" / "resources" \
    / "battle_native_table.generated.c"

# ---- R13-H2 qualified pins (docs/R13H2_BATTLE_SCRIPT_RESOURCE_REPORT.md) ----
MODULE_COUNT_PIN = 2089
PAYLOAD_MODULE_COUNT_PIN = 2081
ALIAS_COUNT_PIN = 8
BYTES_PIN = 90047
EXPORT_ROWS_PIN = 2109
FAMILY_PINS = {
    "battle-script": dict(modules=645, bytes=13592, schema=47,
                          prefix="emerald:battle-script/"),
    "battle-anim-script": dict(modules=658, bytes=63811, schema=48,
                               prefix="emerald:battle-anim-script/"),
    "battle-ai": dict(modules=553, bytes=9303, schema=49,
                      prefix="emerald:battle-ai/"),
    "contest-ai": dict(modules=165, bytes=2524, schema=50,
                       prefix="emerald:contest-ai/"),
    "field-effect-script": dict(modules=68, bytes=817, schema=51,
                                prefix="emerald:field-effect-script/"),
}
# Family -> arena name (arenas.generated.toml).
FAMILY_ARENA = {
    "battle-script": "battle",
    "battle-anim-script": "battle_anim",
    "battle-ai": "battle_ai",
    "contest-ai": "contest_ai",
    "field-effect-script": "field_effect",
}
# Sidecar arena order (layout 0 host order) and the reversed layout 1 order.
ARENA_ORDER_0 = ["battle", "battle_anim", "battle_ai", "contest_ai",
                 "field_effect"]
ARENA_ORDER_1 = list(reversed(ARENA_ORDER_0))

GEN3_RESOURCE_ID_PREFIX = b"gen3-resource-id-v1\x00"

EXPORT_KIND_ENUM = {"offset-zero": 0, "alias": 1}
MAP_KIND_ENUM = {"bytecode": 0, "data": 1, "routing": 2, "empty": 3}


def fail(msg):
    print(f"H3 ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def derive_key(resource_id):
    """Gen3ResourceId_DeriveKey (src/gen3/resources/resource_id.c)."""
    return hashlib.sha256(GEN3_RESOURCE_ID_PREFIX + resource_id.encode()) \
        .digest()


def write_if(path, text, check):
    path = pathlib.Path(path)
    if check:
        if not path.exists():
            fail(f"--check: {path} missing")
        if path.read_text() != text:
            fail(f"--check: {path} differs on regeneration")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def family_of(module_key):
    return module_key[len("emerald:"):].split("/", 1)[0]


def c_bytes(b):
    return ", ".join(f"0x{x:02x}" for x in b)


def c_digest(hexstr):
    return c_bytes(bytes.fromhex(hexstr))


def load_arenas():
    data = tomllib.loads((MODULES_DIR / "arenas.generated.toml").read_text())
    arenas = []
    for a in data["arenas"]:
        holes = [(h["gba_start"], h["length"]) for h in a.get("holes", [])]
        arenas.append(dict(name=a["name"], gba_start=a["gba_start"],
                           gba_end=a["gba_end"],
                           hull_bytes=a["hull_bytes"], holes=holes))
    by_name = {a["name"]: a for a in arenas}
    if [a["name"] for a in arenas] != ARENA_ORDER_0:
        fail(f"arena sidecar order {[a['name'] for a in arenas]} "
             f"!= expected {ARENA_ORDER_0}")
    return by_name, arenas


def load_modules(arenas_by_name):
    """meta/*.toml -> module records. Returns (payload, aliases) lists."""
    payload = []
    aliases = []
    for path in sorted((MODULES_DIR / "meta").glob("*/*.toml")):
        m = tomllib.loads(path.read_text())
        key = m["module_key"]
        fam = family_of(key)
        if fam not in FAMILY_PINS:
            fail(f"unknown family for {key}")
        rec = dict(id=key, family=fam, schema=m["schema"],
                   key=derive_key(key),
                   digest=bytes.fromhex(m["module_digest"]),
                   gba_start=m["region_gba_start"],
                   byte_count=m["region_byte_count"],
                   bytecode_bytes=m["bytecode_bytes"])
        if rec["byte_count"] == 0:
            aliases.append(rec)
        else:
            payload.append(rec)
    payload.sort(key=lambda r: r["id"])
    aliases.sort(key=lambda r: r["id"])
    # Zero-width alias -> canonical payload owner (the payload module whose
    # region contains the shared GBA start; exactly one must exist).
    for alias in aliases:
        owners = [p for p in payload
                  if p["gba_start"] <= alias["gba_start"]
                  < p["gba_start"] + p["byte_count"]]
        if len(owners) != 1:
            fail(f"alias {alias['id']} has {len(owners)} owners, not 1")
        alias["owner_id"] = owners[0]["id"]
    return payload, aliases


def load_boundaries(payload_by_id, aliases_by_id):
    data = tomllib.loads(
        (MODULES_DIR / "boundary_maps.generated.toml").read_text())
    maps = {}
    for m in data["maps"]:
        key = m["module_key"]
        intervals = [(b["payload_offset"], b["length"])
                     for b in m.get("boundaries", [])]
        if key not in payload_by_id and key not in aliases_by_id:
            fail(f"boundary map for unknown module {key}")
        maps[key] = (MAP_KIND_ENUM[m["kind"]], intervals)
    missing = (set(payload_by_id) | set(aliases_by_id)) - set(maps)
    if missing:
        fail(f"modules missing boundary maps: {sorted(missing)[:4]}...")
    return maps


def load_exports(payload_by_id, aliases_by_id):
    data = tomllib.loads(
        (MODULES_DIR / "exports.generated.toml").read_text())
    by_module = {k: [] for k in list(payload_by_id) + list(aliases_by_id)}
    for e in data["exports"]:
        key = e["module_key"]
        if key not in by_module:
            fail(f"export for unknown module {key}")
        by_module[key].append((e["payload_offset"],
                               EXPORT_KIND_ENUM[e["boundary_kind"]]))
    for key, rows in by_module.items():
        rows.sort()
    return by_module


def compute_layouts(arenas_by_name, payload):
    """Per-layout arena sizes/offsets and per-module arena-relative offsets."""
    # layout 0: GBA-preserving, arena order 0.
    off0 = {}
    for a in ARENA_ORDER_0:
        off0[a] = sum(arenas_by_name[x]["hull_bytes"]
                      for x in ARENA_ORDER_0[:ARENA_ORDER_0.index(a)])
    # layout 1: tight pack (module-id order), arena order reversed.
    # Module offsets are ARENA-RELATIVE (the seam adds the arena base);
    # only the arena layout offsets are combined-buffer offsets.
    pack = {a: sorted((p for p in payload
                       if p["arena"] == a), key=lambda r: r["id"])
            for a in ARENA_ORDER_0}
    tight_size = {a: sum(p["byte_count"] for p in pack[a])
                  for a in ARENA_ORDER_0}
    off1 = {}
    for a in ARENA_ORDER_1:
        off1[a] = sum(tight_size[x]
                      for x in ARENA_ORDER_1[:ARENA_ORDER_1.index(a)])
    module_off1 = {}
    for a in ARENA_ORDER_0:
        cursor = 0
        for p in pack[a]:
            module_off1[p["id"]] = cursor
            cursor += p["byte_count"]
    arena_layout = {}
    for a in ARENA_ORDER_0:
        arena_layout[a] = dict(
            layout_size=[arenas_by_name[a]["hull_bytes"], tight_size[a]],
            layout_offset=[off0[a], off1[a]])
    return arena_layout, module_off1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="verify regeneration is a no-op diff")
    args = parser.parse_args()

    arenas_by_name, arenas = load_arenas()
    payload, aliases = load_modules(arenas_by_name)
    payload_by_id = {p["id"]: p for p in payload}
    aliases_by_id = {a["id"]: a for a in aliases}
    if len(payload) != PAYLOAD_MODULE_COUNT_PIN:
        fail(f"payload modules {len(payload)} != pin {PAYLOAD_MODULE_COUNT_PIN}")
    if len(aliases) != ALIAS_COUNT_PIN:
        fail(f"alias modules {len(aliases)} != pin {ALIAS_COUNT_PIN}")
    total_bytes = sum(p["byte_count"] for p in payload)
    if total_bytes != BYTES_PIN:
        fail(f"canonical bytes {total_bytes} != pin {BYTES_PIN}")

    # Family/arena attribution + pins.
    fam_idx = {name: i for i, name in enumerate(FAMILY_PINS)}
    arena_idx = {a["name"]: i for i, a in enumerate(arenas)}
    for rec in payload + aliases:
        rec["family_idx"] = fam_idx[rec["family"]]
        rec["arena"] = FAMILY_ARENA[rec["family"]]
        rec["arena_idx"] = arena_idx[rec["arena"]]
        if rec["arena"] not in arenas_by_name:
            fail(f"module {rec['id']} arena {rec['arena']} unknown")
    for fam, pin in FAMILY_PINS.items():
        mods = [p for p in payload + aliases if p["family"] == fam]
        if len(mods) != pin["modules"]:
            fail(f"family {fam} modules {len(mods)} != pin {pin['modules']}")
        byt = sum(p["byte_count"] for p in mods)
        if byt != pin["bytes"]:
            fail(f"family {fam} bytes {byt} != pin {pin['bytes']}")

    maps = load_boundaries(payload_by_id, aliases_by_id)
    exports = load_exports(payload_by_id, aliases_by_id)
    export_rows = sum(len(v) for v in exports.values())
    if export_rows != EXPORT_ROWS_PIN:
        fail(f"export rows {export_rows} != pin {EXPORT_ROWS_PIN}")

    # Owner-index for aliases (payload id -> table index assigned below).
    for alias in aliases:
        alias["owner_module_idx"] = None  # filled after ordering

    # Ordering: payload modules sorted by id; aliases appended sorted by id.
    modules = payload + aliases
    payload_idx = {p["id"]: i for i, p in enumerate(payload)}
    for alias in aliases:
        alias["owner_module_idx"] = payload_idx[alias["owner_id"]]

    arena_layout, module_off1 = compute_layouts(arenas_by_name, payload)

    # Per-module layout offsets (arena-relative host offsets).
    for p in payload:
        a = arenas_by_name[p["arena"]]
        p["layout_off"] = [p["gba_start"] - a["gba_start"],
                           module_off1[p["id"]]]
    for alias in aliases:
        alias["layout_off"] = [0, 0]  # zero-width: no host span

    # Flat boundary + export arrays with per-module first/count.
    boundaries = []
    exports_flat = []
    for rec in modules:
        kind, intervals = maps[rec["id"]]
        rec["map_kind"] = kind
        rec["boundary_first"] = len(boundaries)
        for off, length in intervals:
            boundaries.append((off, length))
        rec["boundary_count"] = len(intervals)
        rec["export_first"] = len(exports_flat)
        for off, ekind in exports[rec["id"]]:
            exports_flat.append((off, ekind))
        rec["export_count"] = len(exports[rec["id"]])

    # Holes: (arena_idx, gba_start, length).
    holes = []
    for a in arenas:
        for hs, ln in a["holes"]:
            holes.append((arena_idx[a["name"]], hs, ln))

    total_boundary_count = len(boundaries)

    # ---- header ----
    h = []
    h.append("/* Generated by tools/gen3_resources/battle_family/"
             "h3_generate.py.")
    h.append(" * Do not edit by hand; re-run the generator (regeneration must")
    h.append(" * be a no-op diff).")
    h.append(" *")
    h.append(" * R13-H3 shadow seam + state adapter compilation table, derived")
    h.append(" * from the R13-H2 sidecars. Physical layouts are generation")
    h.append(" * choices; semantic identity (family/module/offset) is not.")
    h.append(" */")
    h.append("")
    h.append("#ifndef EMERALD_RESOURCES_BATTLE_NATIVE_GENERATED_H")
    h.append("#define EMERALD_RESOURCES_BATTLE_NATIVE_GENERATED_H")
    h.append("")
    h.append("#include <stdint.h>")
    h.append("")
    h.append("#define EMERALD_BATTLE_FAMILY_COUNT %du" % len(FAMILY_PINS))
    h.append("#define EMERALD_BATTLE_MODULE_COUNT %du" % len(modules))
    h.append("#define EMERALD_BATTLE_PAYLOAD_MODULE_COUNT %du" % len(payload))
    h.append("#define EMERALD_BATTLE_ALIAS_COUNT %du" % len(aliases))
    h.append("#define EMERALD_BATTLE_BOUNDARY_COUNT %du" % total_boundary_count)
    h.append("#define EMERALD_BATTLE_EXPORT_ROW_COUNT %du" % len(exports_flat))
    h.append("#define EMERALD_BATTLE_CANONICAL_BYTES %du" % total_bytes)
    h.append("#define EMERALD_BATTLE_LAYOUT_COUNT 2u")
    h.append("")
    h.append("enum EmeraldBattleNativeFamily")
    h.append("{")
    for name in FAMILY_PINS:
        h.append("    EMERALD_BATTLE_FAMILY_%s = %du,"
                 % (name.replace("-", "_").upper(), fam_idx[name]))
    h.append("};")
    h.append("")
    h.append("enum EmeraldBattleNativeMapKind")
    h.append("{")
    h.append("    EMERALD_BATTLE_MAP_BYTECODE = 0,")
    h.append("    EMERALD_BATTLE_MAP_DATA = 1,")
    h.append("    EMERALD_BATTLE_MAP_ROUTING = 2,")
    h.append("    EMERALD_BATTLE_MAP_EMPTY = 3,")
    h.append("};")
    h.append("")
    h.append("enum EmeraldBattleNativeExportKind")
    h.append("{")
    h.append("    EMERALD_BATTLE_EXPORT_OFFSET_ZERO = 0,")
    h.append("    EMERALD_BATTLE_EXPORT_ALIAS = 1,")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeBoundary")
    h.append("{")
    h.append("    uint32_t payloadOffset;")
    h.append("    uint32_t length;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeExport")
    h.append("{")
    h.append("    uint32_t payloadOffset;")
    h.append("    uint8_t kind; /* EmeraldBattleNativeExportKind */")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeArena")
    h.append("{")
    h.append("    const char *name;")
    h.append("    uint32_t gbaStart;")
    h.append("    uint32_t gbaEnd; /* exclusive */")
    h.append("    uint32_t hullBytes;")
    h.append("    uint32_t layoutSize[EMERALD_BATTLE_LAYOUT_COUNT];")
    h.append("    uint32_t layoutOffset[EMERALD_BATTLE_LAYOUT_COUNT];")
    h.append("    uint32_t holeFirst;")
    h.append("    uint32_t holeCount;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeModule")
    h.append("{")
    h.append("    const char *id;")
    h.append("    uint8_t key[32];")
    h.append("    uint32_t schema;")
    h.append("    uint32_t family; /* EmeraldBattleNativeFamily */")
    h.append("    uint32_t arena;")
    h.append("    uint32_t gbaStart;")
    h.append("    uint32_t byteCount;")
    h.append("    uint8_t digest[32];")
    h.append("    uint32_t layoutOffset[EMERALD_BATTLE_LAYOUT_COUNT];")
    h.append("    uint32_t boundaryFirst;")
    h.append("    uint32_t boundaryCount;")
    h.append("    uint32_t exportFirst;")
    h.append("    uint32_t exportCount;")
    h.append("    uint32_t mapKind; /* EmeraldBattleNativeMapKind */")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeAlias")
    h.append("{")
    h.append("    const char *id;")
    h.append("    uint32_t family;")
    h.append("    uint32_t ownerModule; /* canonical payload owner */")
    h.append("    uint32_t gbaStart;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeHole")
    h.append("{")
    h.append("    uint32_t arena;")
    h.append("    uint32_t gbaStart;")
    h.append("    uint32_t length;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeFamilyInfo")
    h.append("{")
    h.append("    const char *name;")
    h.append("    const char *prefix;")
    h.append("    uint32_t schema;")
    h.append("};")
    h.append("")
    h.append("struct EmeraldBattleNativeTable")
    h.append("{")
    h.append("    uint32_t moduleCount;")
    h.append("    uint32_t payloadModuleCount;")
    h.append("    uint32_t aliasCount;")
    h.append("    uint32_t boundaryCount;")
    h.append("    uint32_t exportRowCount;")
    h.append("    uint32_t arenaCount;")
    h.append("    const struct EmeraldBattleNativeFamilyInfo *families;")
    h.append("    const struct EmeraldBattleNativeArena *arenas;")
    h.append("    const struct EmeraldBattleNativeModule *modules;")
    h.append("    const struct EmeraldBattleNativeBoundary *boundaries;")
    h.append("    const struct EmeraldBattleNativeExport *exports;")
    h.append("    const struct EmeraldBattleNativeAlias *aliases;")
    h.append("    const struct EmeraldBattleNativeHole *holes;")
    h.append("};")
    h.append("")
    h.append("extern const struct EmeraldBattleNativeTable "
             "kEmeraldBattleCompatTable;")
    h.append("")
    h.append("#endif /* EMERALD_RESOURCES_BATTLE_NATIVE_GENERATED_H */")
    header = "\n".join(h) + "\n"

    # ---- C table ----
    c = []
    c.append("/* Generated by tools/gen3_resources/battle_family/"
             "h3_generate.py.")
    c.append(" * Do not edit by hand; re-run the generator (regeneration must")
    c.append(" * be a no-op diff).")
    c.append(" */")
    c.append("")
    c.append("#include \"emerald/resources/battle_native.generated.h\"")
    c.append("")
    c.append("static const struct EmeraldBattleNativeFamilyInfo "
             "sFamilies[EMERALD_BATTLE_FAMILY_COUNT] = {")
    for name in FAMILY_PINS:
        pin = FAMILY_PINS[name]
        c.append('    {"%s", "%s", %du},' % (name, pin["prefix"],
                                             pin["schema"]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeHole "
             "sHoles[] = {")
    for ai, hs, ln in holes:
        c.append("    {%du, 0x%x, %du}," % (ai, hs, ln))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeArena sArenas[] = {")
    for a in arenas:
        ai = arena_idx[a["name"]]
        first = sum(1 for h in holes if h[0] < ai)
        count = sum(1 for h in holes if h[0] == ai)
        la = arena_layout[a["name"]]
        c.append("    {\"%s\", 0x%x, 0x%x, %du, {%du, %du}, {%du, %du}, "
                 "%du, %du},"
                 % (a["name"], a["gba_start"], a["gba_end"],
                    a["hull_bytes"], la["layout_size"][0],
                    la["layout_size"][1], la["layout_offset"][0],
                    la["layout_offset"][1], first, count))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeBoundary "
             "sBoundaries[] = {")
    for off, length in boundaries:
        c.append("    {%du, %du}," % (off, length))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeExport sExports[] = {")
    for off, ekind in exports_flat:
        c.append("    {%du, %du}," % (off, ekind))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeModule sModules[] = {")
    for rec in modules:
        c.append('    {"%s",' % rec["id"])
        c.append("     {%s}," % c_bytes(rec["key"]))
        c.append("     %du, %du, %du, 0x%x, %du," % (rec["schema"],
                                                     rec["family_idx"],
                                                     rec["arena_idx"],
                                                     rec["gba_start"],
                                                     rec["byte_count"]))
        c.append("     {%s}," % c_digest(rec["digest"].hex()))
        c.append("     {%du, %du}, %du, %du, %du, %du, %du},"
                 % (rec["layout_off"][0], rec["layout_off"][1],
                    rec["boundary_first"], rec["boundary_count"],
                    rec["export_first"], rec["export_count"],
                    rec["map_kind"]))
    c.append("};")
    c.append("")
    c.append("static const struct EmeraldBattleNativeAlias sAliases[] = {")
    for rec in aliases:
        c.append('    {"%s", %du, %du, 0x%x},'
                 % (rec["id"], rec["family_idx"], rec["owner_module_idx"],
                    rec["gba_start"]))
    c.append("};")
    c.append("")
    c.append("const struct EmeraldBattleNativeTable kEmeraldBattleCompatTable = {")
    c.append("    %du, %du, %du, %du, %du, %du," % (len(modules),
                                                     len(payload),
                                                     len(aliases),
                                                     total_boundary_count,
                                                     len(exports_flat),
                                                     len(arenas)))
    c.append("    sFamilies, sArenas, sModules, sBoundaries, sExports,")
    c.append("    sAliases, sHoles,")
    c.append("};")
    table = "\n".join(c) + "\n"

    write_if(HEADER_OUT, header, args.check)
    write_if(TABLE_OUT, table, args.check)
    print(f"H3 table: {len(modules)} modules ({len(payload)} payload, "
          f"{len(aliases)} aliases), {total_boundary_count} boundaries, "
          f"{len(exports_flat)} export rows, {len(arenas)} arenas, "
          f"{total_bytes} B")


if __name__ == "__main__":
    main()
