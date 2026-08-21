#!/usr/bin/env python3
"""R13-G3 fault-injection table mutator (test-only).

Reads src/emerald/resources/script_native_table.generated.c, applies
ONE named mutation to a specific generated row (the plan sec 17 fault
matrix), and writes the mutated copy to the given path. The seam is
compiled against the mutated copy; its phase-1 structural gates and
parity oracle must refuse with the status named in the fault spec.

Row initializers are single lines (the generator's emit_array contract)
so the mutator edits one line per fault. Field order per struct:

  Module     {id, sym, {digest}, schema, rstart, size, arena,
              sfirst, sn, efirst, en, rfirst, rn, bfirst, bn, kind, emb}
  Segment    {gba, byteCount, payloadOffset, kind}
  Export     {moduleIndex, payloadOffset, gba, boundaryKind, kind, name}
  Reloc      {moduleIndex, operandOff, encGba, cls, kind, key, label, to, tpo}
  Routing    {gba, table, cls, kind, tg, key, label, to, tpo}
  Dynamic    {gba, cls, kind, key, label, to, tpo, bk}
  StdScript  {moduleIndex, exportIndex, payloadOffset, encGba, rom, bk, slot}
  FBinding   {kind, boundaryKind, sameMap, mapSymbol, mapKey, gbaTarget,
              moduleIndex, exportIndex, payloadOffset}
  Mart       {moduleIndex, payloadOffset, gba, label, byteCount, itemCount}
  RamTarget  {gba, region, sourceGbaOffset}
  Boundary   {payloadOffset, length, pad}
"""

import re
import sys

ARRAYS = {
    "bridges": "kScriptBridges",
    "modules": "kScriptModules",
    "segments": "kScriptSegments",
    "exports": "kScriptExports",
    "relocs": "kScriptRelocs",
    "routing": "kScriptRoutingRelocs",
    "dynamic": "kScriptDynamicTargets",
    "std": "kScriptStdScripts",
    "fbind": "kScriptFBindings",
    "marts": "kScriptMarts",
    "ram": "kScriptRamTargets",
    "boundaries": "kScriptBoundaries",
}

# Fault spec: (array, row index, kind of edit)
FAULTS = {
    "seg-overlap":      ("segments", 10, "field", 2, 1),
    "seg-gap":          ("segments", 10, "field", 1, -1),
    "seg-kind":         ("segments", 10, "setfield", 3, 2),
    "export-outside":   ("exports", 20, "setfield", 1, 0xFFFFFFF0),
    "export-dup":       ("exports", 20, "copyfield", 1, -1),
    "reloc-raw":        ("relocs", 1000, "flipfield", 2, 0),
    "reloc-overlap":    ("relocs", 1000, "copyfield", 1, -1),
    "reloc-class":      ("relocs", 1000, "addfield", 3, 1),
    "text-missing":     ("relocs", "FIRST_TEXT", "setfield", 5, 0),
    "boundary-dup":     ("boundaries", 2000, "copyfield", 0, -1),
    "mart-sentinel":    ("marts", 0, "addfield", 4, 2),
    "f-missing":        ("fbind", 600, "setfield", 8, 0xFFFFFFFE),
    "std-missing":      ("std", 0, "addfield", 2, 1),
    "ram-bad":          ("ram", 0, "setfield", 0, 0x02000000),
    "bridge-bad":       ("bridges", 0, "setfield", 3, 9),
    "dyn-gap":          ("dynamic", 5000, "copyfield", 0, -1),
    "module-digest":    ("modules", 0, "flip-digest", 0, 0),
    "module-arena":     ("modules", 1, "addfield", 6, 16),
    "f-routing-object": ("fbind", 600, "setfield", 1, 2),
    "export-missing":   ("exports", 20, "addfield", 1, 1),
}

# Expected refusal status per fault (the fault driver asserts it).
EXPECTED = {
    "seg-overlap":      "EMERALD_SCRIPT_ERR_SEGMENT_INVALID",
    "seg-gap":          "EMERALD_SCRIPT_ERR_SEGMENT_INVALID",
    "seg-kind":         "EMERALD_SCRIPT_ERR_SEGMENT_INVALID",
    "export-outside":   "EMERALD_SCRIPT_ERR_EXPORT_INVALID",
    "export-dup":       "EMERALD_SCRIPT_ERR_EXPORT_INVALID",
    "reloc-raw":        "EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED",
    "reloc-overlap":    "EMERALD_SCRIPT_ERR_RELOC_INVALID",
    "reloc-class":      "EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED",
    "text-missing":     "EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED",
    "boundary-dup":     "EMERALD_SCRIPT_ERR_BOUNDARY_INVALID",
    "mart-sentinel":    "EMERALD_SCRIPT_ERR_SEGMENT_INVALID",
    "f-missing":        "EMERALD_SCRIPT_ERR_EXPORT_INVALID",
    "std-missing":      "EMERALD_SCRIPT_ERR_EXPORT_INVALID",
    "ram-bad":          "EMERALD_SCRIPT_ERR_TABLE_MISMATCH",
    "bridge-bad":       "EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT",
    "dyn-gap":          "EMERALD_SCRIPT_ERR_TABLE_MISMATCH",
    "module-digest":    "EMERALD_SCRIPT_ERR_TABLE_MISMATCH",
    "module-arena":     "EMERALD_SCRIPT_ERR_TABLE_MISMATCH",
    "f-routing-object": "EMERALD_SCRIPT_ERR_TABLE_MISMATCH",
    "export-missing":   "EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED",
}

def row_body(line):
    """The initializer body: between the first '{' and the LAST '}' of
    the single-line row (nested brace lists keep their closing braces)."""
    return line[line.index("{") + 1:line.rindex("}")]


def split_fields(body):
    """Split a braced initializer body by top-level commas. A nested
    brace (module digest bytes / bridge bytes) is replaced by a
    placeholder before splitting and restored at its own position."""
    nested = re.search(r"\{[^}]*\}", body)
    if nested:
        body = body[:nested.start()] + "NESTED" + body[nested.end():]
        has_nested = True
    else:
        has_nested = False
    fields = [f.strip() for f in body.split(",")]
    return fields, has_nested, nested


def join_fields(fields, has_nested, nested):
    if has_nested:
        idx = fields.index("NESTED")
        fields[idx] = nested.group(0)
    return ", ".join(fields)


def mutate(src, fault):
    spec = FAULTS[fault]
    arr_name, row_idx, op = spec[0], spec[1], spec[2]
    args = spec[3:]
    arr_cname = ARRAYS.get(arr_name) or arr_name
    marker = f"static const struct EmeraldScriptNative"
    start = src.index(f" {arr_cname}[")
    seg = src[start:src.index("};", start)]
    lines = seg.split("\n")
    row_lines = [ln for ln in lines if ln.strip().startswith("{")]
    if row_idx == "FIRST_TEXT":
        # first reloc row whose class field (index 3) is TEXT (1u)
        row_idx = next(i for i, ln in enumerate(row_lines)
                       if split_fields(row_body(ln))[0][3] == "1u")
    if row_idx >= len(row_lines):
        raise SystemExit(f"fault {fault}: row {row_idx} out of range "
                         f"({len(row_lines)} rows)")
    target = row_lines[row_idx]
    body = row_body(target)
    fields, has_nested, nested = split_fields(body)

    def to_int(s):
        return int(s.rstrip("u"), 0)

    if op == "field":
        fidx, delta = args
        fields[fidx] = f"{to_int(fields[fidx]) + delta}u"
    elif op == "setfield":
        fidx, value = args
        fields[fidx] = f"{value}u"
    elif op == "copyfield":
        fidx, delta = args
        fields[fidx] = row_lines[row_idx + delta].strip().strip("{").strip("},").split(",")[fidx].strip()
    elif op == "addfield":
        fidx, delta = args
        fields[fidx] = f"{to_int(fields[fidx]) + delta}u"
    elif op == "flipfield":
        fidx, delta = args
        v = to_int(fields[fidx])
        fields[fidx] = hex(v ^ (1 << delta))
    elif op == "flip-digest":
        # flip the last byte of the module digest (restored nested list)
        blob = nested.group(0)
        parts = [p.strip() for p in blob.strip("{}").split(",")]
        parts[-1] = hex(int(parts[-1], 0) ^ 0xFF)
        nested2 = "{" + ", ".join(parts) + "}"
        new = target.replace(nested.group(0), nested2)
        return src.replace(target, new)

    new = "{" + join_fields(fields, has_nested, nested) + "},"
    new_line = target[:target.index("{")] + new
    if new_line != target:
        src = src.replace(target, new_line)
    else:
        raise SystemExit(f"fault {fault}: mutation produced no change")
    return src


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: fault_script_table.py <fault> <src.c> <dst.c>")
    fault, src_path, dst_path = sys.argv[1], sys.argv[2], sys.argv[3]
    if fault not in FAULTS:
        raise SystemExit(f"unknown fault {fault} (known: {sorted(FAULTS)})")
    src = open(src_path).read()
    mutated = mutate(src, fault)
    open(dst_path, "w").write(mutated)
    print(f"{fault} -> {EXPECTED[fault]}")


if __name__ == "__main__":
    main()
