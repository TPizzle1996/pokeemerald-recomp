#!/usr/bin/env python3
"""R15 UI family: generate emerald_ui_populate.c + ui_struct_population.h
from the converted consumer files.

The converted files follow the pattern:

    #if defined(NATIVE_LINUX)
    struct CompressedSpriteSheet sX = {NULL, 0x180, TAG_X};   (mutable,
        non-static, all scalars retained, gated pointers NULL)
    #else
    static const struct CompressedSpriteSheet sX =            (byte-identical
        {                                                       to HEAD)
            .data = gY, .size = 0x180, .tag = TAG_X
        };
    #endif

This generator parses the GBA (#else) rows to learn, per converted
struct, which pointer field of which row originally referenced which
gated symbol, and emits EmeraldUICompat_PopulateStructs() assignments
reusing those expressions verbatim (the ui_accessors.generated.h macros
translate the legacy symbol names into UI_Get() calls on NATIVE_LINUX,
so `name[i].data = &gPal[0x10]` becomes a correct pack-backed pointer
offset). EmeraldUICompat_ClearStructPointers() NULLs the same fields.

Usage: gen_ui_populate.py [--check]
"""
import re
import sys
import subprocess

ROOT = "/home/tristen/work/pokeemerald-recomp"
POPULATE_C = f"{ROOT}/src/emerald/resources/emerald_ui_populate.c"
POPULATE_H = f"{ROOT}/include/emerald/resources/ui_struct_population.h"

# The files the R15 UI physical-removal conversion touched.
FILES = [
    "src/battle_anim_smokescreen.c", "src/battle_arena.c", "src/battle_bg.c",
    "src/battle_dome.c", "src/battle_gfx_sfx_util.c", "src/battle_interface.c",
    "src/battle_pyramid_bag.c", "src/berry_blender.c", "src/cable_car.c",
    "src/contest.c", "src/contest_util.c", "src/data/party_menu.h",
    "src/data/trade.h", "src/easy_chat.c", "src/frontier_pass.c",
    "src/hall_of_fame.c", "src/item_menu_icons.c", "src/menu_helpers.c",
    "src/money.c", "src/naming_screen.c", "src/pokeblock.c",
    "src/pokeblock_feed.c", "src/pokedex.c", "src/pokedex_area_screen.c",
    "src/pokemon_summary_screen.c", "src/pokenav_main_menu.c",
    "src/pokenav_menu_handler_gfx.c", "src/pokenav_region_map.c",
    "src/roulette.c", "src/tileset_anims.c", "src/title_screen.c",
    "src/trainer_card.c", "src/union_room_chat.c", "src/use_pokeblock.c",
    "src/wallclock.c",
]

# First struct field for the struct kinds used positionally.
FIRST_FIELD = {
    "CompressedSpriteSheet": "data",
    "CompressedSpriteSheetNoSize": "data",
    "CompressedSpritePalette": "data",
    "SpriteSheet": "data",
    "SpritePalette": "data",
    "TilemapCtrl": "gfx",
}

BLOCK_RE = re.compile(
    r"#if defined\(NATIVE_LINUX\)\n(.*?)#else\n(.*?)#endif",
    re.S)
STRUCT_DECL_RE = re.compile(
    r"^\s*struct\s+(\w+)\s+(\w+)(?:\s*\[([^\]\n]+)\])?\s*(?:=|\{|;|\n)", re.M)
PTR_DECL_RE = re.compile(
    r"^\s*((?:const\s+)?\w+)\s*\*\s*(?:const\s+)?(\w+)\s*(?:\[([^\]\n]+)\])?\s*(?:=|\n)",
    re.M)
GBA_DECL_RE = re.compile(
    r"(?:static\s+)?(?:const\s+)?struct\s+(\w+)\s+(\w+)(?:\s*\[([^\]\n=]*)\])?\s*=\s*(\{(?:[^{}]|\{[^{}]*\})*\})",
    re.S)
GBA_PTR_RE = re.compile(
    r"(?:static\s+)?const\s+(\w+)\s*\*\s*const\s+(\w+)\s*\[\s*([^\]\n]*)\s*\]\s*=\s*(\{(?:[^{}]|\{[^{}]*\})*\})",
    re.S)
FIELD_RE = re.compile(r"\.(\w+)\s*=\s*(.*?)(?:,|$)", re.S)


def gated_symbols():
    out = set()
    for line in open(
            f"{ROOT}/include/emerald/resources/ui_accessors.generated.h"):
        m = re.match(r"#define (\w+) ", line)
        if m:
            out.add(m.group(1))
    return out


def split_rows(body):
    """Split an initializer-list body (outer braces stripped) into per-row
    contents, top-level braces removed."""
    body = body.strip()
    if body.startswith("{"):
        body = body[1:]
    if body.endswith("}"):
        body = body[:-1]
    rows = []
    cur = ""
    depth = 0
    for c in body:
        if c == "{":
            if depth == 0:
                cur = ""
            else:
                cur += c
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                rows.append(cur)
                cur = ""
            else:
                cur += c
        else:
            cur += c
    if cur.strip():
        rows.append(cur)  # scalar initializer (no inner braces)
    return rows


def parse_gba_rows(struct_type, body, gated):
    """Yield (designator_or_None, field, expression) for rows whose pointer
    field references a gated symbol."""
    out = []
    pos = 0
    for raw in split_rows(body):
        raw = strip_comments(raw).strip()
        if not raw:
            pos += 1
            continue
        designator = None
        m = re.match(r"\[\s*([^\]]+)\s*\]\s*=\s*(.*)", raw, re.S)
        if m:
            designator = m.group(1).strip()
            inner = m.group(2).strip()
        else:
            inner = raw
        # named fields
        named = dict(FIELD_RE.findall(inner))
        if named:
            for field, expr in named.items():
                expr = expr.strip().rstrip(",").strip()
                if has_gated(expr, gated):
                    out.append((designator, pos, field, expr))
        else:
            # positional: first element is the pointer
            first = inner.split(",", 1)[0].strip()
            if has_gated(first, gated):
                field = FIRST_FIELD.get(struct_type)
                if field is None:
                    raise SystemExit(
                        f"positional row for unknown struct {struct_type}: {inner}")
                out.append((designator, pos, field, first))
        pos += 1
    return out


def parse_file(path, gated):
    """Yield Struct records: (type, name, size_or_None, [(des, pos, field, expr)])."""
    text = open(path).read()
    out = []
    for block in BLOCK_RE.finditer(text):
        native_branch = block.group(1)
        gba_branch = block.group(2)
        native_structs = []
        for line in native_branch.split("\n"):
            m = STRUCT_DECL_RE.match(line)
            if m:
                native_structs.append(
                    {"kind": "struct", "type": m.group(1), "name": m.group(2),
                     "size": m.group(3)})
                continue
            m = PTR_DECL_RE.match(line)
            if m:
                native_structs.append(
                    {"kind": "ptr", "type": m.group(1), "name": m.group(2),
                     "size": m.group(3)})
        gba_structs = dict((m.group(2), m)
                           for m in GBA_DECL_RE.finditer(gba_branch))
        gba_ptrs = dict((m.group(2), m)
                        for m in GBA_PTR_RE.finditer(gba_branch))
        for rec in native_structs:
            g = (gba_structs if rec["kind"] == "struct" else gba_ptrs).get(
                rec["name"])
            if g is None:
                raise SystemExit(
                    f"{path}: no GBA definition for native {rec['name']}")
            if rec["kind"] == "struct":
                # file-local struct kinds: capture their definition for the
                # extern header (the populate TU cannot see the source file)
                m = re.search(
                    r"struct\s+" + rec["type"] + r"\s*\{([^}]*)\};",
                    text, re.S)
                if m:
                    rec["local_def"] = m.group(0)
            if rec["kind"] == "struct":
                rows = parse_gba_rows(rec["type"], g.group(4), gated)
            else:
                rows = parse_ptr_rows(g.group(4), gated)
            rec.update({"path": path, "rows": rows})
            out.append(rec)
    return out


def has_gated(expr, gated):
    for s in gated:
        if re.search(r"(?<![\w])" + re.escape(s) + r"(?![\w])", expr):
            return True
    return False


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def parse_ptr_rows(body, gated):
    """Pointer tables: rows are bare expressions separated by commas."""
    body = strip_comments(body).strip()
    if body.startswith("{"):
        body = body[1:]
    if body.endswith("}"):
        body = body[:-1]
    out = []
    pos = 0
    for raw in body.split(","):
        raw = raw.strip()
        if not raw:
            pos += 1
            continue
        designator = None
        m = re.match(r"\[\s*([^\]]+)\s*\]\s*=\s*(.*)", raw, re.S)
        if m:
            designator = m.group(1).strip()
            expr = m.group(2).strip()
        else:
            expr = raw
        if has_gated(expr, gated):
            out.append((designator, pos, None, expr))
        pos += 1
    return out


def assignment(rec, des, pos, field, expr, safe):
    if rec["size"]:
        idx, cmt = des_to_index(des, pos, safe)
    else:
        idx, cmt = "", ""
    if field:
        return f"    {rec['name']}{idx}.{field} = {expr};{cmt}"
    return f"    {rec['name']}{idx} = {expr};{cmt}"


def safe_designator_ids():
    """Identifiers declared in include/ headers (usable from the populate TU)."""
    import glob
    ids = set()
    for h in glob.glob(f"{ROOT}/include/**/*.h", recursive=True):
        for m in re.finditer(r"\b([A-Z][A-Za-z0-9_]{2,})\b", open(h).read()):
            ids.add(m.group(1))
    return ids


def des_to_index(des, pos, safe):
    """Return (index_text, comment) for a designator expression."""
    if des is None:
        return f"[{pos}]", ""
    idents = set(re.findall(r"[A-Za-z_]\w*", des))
    if idents <= safe:
        return f"[{des}]", ""
    return f"[{pos}]", f" /* [{des}] */"


def main():
    check = "--check" in sys.argv
    safe = safe_designator_ids()
    gated = gated_symbols()
    records = []
    for f in FILES:
        records.extend(parse_file(f"{ROOT}/{f}", gated))

    # extern header
    hlines = [
        "/* Generated by R15 UI family generator (gen_ui_populate.py)",
        " * Do not edit by hand.",
        " *",
        " * Extern declarations for the mutable native UI structs published",
        " * from the pack by EmeraldUICompat_PopulateStructs(). */",
        "#ifndef EMERALD_RESOURCES_UI_STRUCT_POPULATION_H",
        "#define EMERALD_RESOURCES_UI_STRUCT_POPULATION_H",
        "",
        "#if defined(NATIVE_LINUX)",
        '#include "sprite.h"',
        '#include "bg.h"',
    ]
    emitted_local = set()
    for rec in records:
        arr = "[]" if rec["size"] else ""
        if rec["kind"] == "struct":
            if "local_def" in rec and rec["type"] not in emitted_local:
                hlines.append(
                    "/* file-local struct kind, definition mirrored from "
                    f"{rec['path']} */")
                hlines.append(rec["local_def"])
                hlines.append("")
                emitted_local.add(rec["type"])
            hlines.append(
                f"extern struct {rec['type']} {rec['name']}{arr};")
        else:
            hlines.append(
                f"extern const {rec['type']} *{rec['name']}{arr};")
    hlines += [
        "",
        "void EmeraldUICompat_PopulateStructs(void);",
        "void EmeraldUICompat_ClearStructPointers(void);",
        "#endif",
        "#endif",
        "",
    ]

    # populate C
    clines = [
        "/* Generated by R15 UI family generator (gen_ui_populate.py)",
        " * Do not edit by hand.",
        " *",
        " * Populates every mutable native UI struct's pointer fields from the",
        " * EmeraldUICompat image. The expressions below are the original GBA",
        " * initializer expressions; on NATIVE_LINUX the accessor macros in",
        " * ui_accessors.generated.h translate the legacy symbol names into",
        " * pack-backed UI_Get() pointers, including &gPal[0x10]-style",
        " * offsets. */",
        '#include "global.h"',
        '#include "graphics.h"',
        '#include "pokeblock.h"',
        '#include "emerald/resources/ui_struct_population.h"',
        "",
        "#if defined(NATIVE_LINUX)",
        "",
        "void EmeraldUICompat_PopulateStructs(void)",
        "{",
    ]
    clears = [
        "",
        "void EmeraldUICompat_ClearStructPointers(void)",
        "{",
    ]
    for rec in records:
        clines.append(f"    /* {rec['path']} */")
        clears.append(f"    /* {rec['path']} */")
        for des, pos, field, expr in rec["rows"]:
            clines.append(assignment(rec, des, pos, field, expr, safe))
            if rec["size"]:
                idx, cmt = des_to_index(des, pos, safe)
            else:
                idx, cmt = "", ""
            if field:
                clears.append(f"    {rec['name']}{idx}.{field} = NULL;{cmt}")
            else:
                clears.append(f"    {rec['name']}{idx} = NULL;{cmt}")
    clines.append("}")
    clears.append("}")
    clears.append("")
    clines.extend(clears)
    clines.append("#endif")
    clines.append("")
    ctext = "\n".join(clines)
    htext = "\n".join(hlines)

    if check:
        if open(POPULATE_C).read() != ctext or open(POPULATE_H).read() != htext:
            raise SystemExit("populate outputs differ from generator output")
        print("gen_ui_populate --check: byte-identical")
        return
    open(POPULATE_H, "w").write(htext)
    open(POPULATE_C, "w").write(ctext)
    n_rows = sum(len(r["rows"]) for r in records)
    print(f"regenerated: {len(records)} structs, {n_rows} pointer rows")


if __name__ == "__main__":
    main()
