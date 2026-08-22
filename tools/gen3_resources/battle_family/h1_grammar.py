"""h1_grammar.py — machine-readable opcode grammars for the five R13-H
battle-family VMs, generated from the qualified macro files and cross-checked
against the C interpreter tables.

Sources:
  battle VM        asm/macros/battle_script.inc        + gBattleScriptingCommandsTable
  battle anim VM   asm/macros/battle_anim_script.inc   + sScriptCmdTable
  battle AI VM     asm/macros/battle_ai_script.inc     + sBattleAICmdTable
  contest AI VM    asm/macros/contest_ai_script.inc    + sContestAICmdTable
  field-effect VM  asm/macros/field_effect_script.inc  + gFieldEffectScriptFuncs

Grammar record per opcode encoding:
  {opcode, command, size, operands[] {name, width, role}, flow,
   mayBlock, maySuspend, return/call semantics, aliases}

Roles (assigned by the analyzer from handler evidence; the grammar only
records the macro-level emission role):
  opcode / operand / scalarb / scalars / scalarw / ptr / textid / animid / battler
"""

import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
# Grammar MUST come from the qualified reference macros (the recomp repo's
# macro files are a newer pret revision: `.int` vs `.4byte`, `:req` markers,
# renamed labels). The qualified objects were built from the reference set.
REF = os.path.join(os.path.dirname(REPO), "pokeemerald-reference")
MACRO_DIR = os.path.join(REF, "asm", "macros")

# (macro file, C handler table file, table name, expected opcode range)
FAMILIES = [
    ("battle",         "battle_script.inc",       "src/battle_script_commands.c", "gBattleScriptingCommandsTable", 0x00, 0xF8),
    ("battle_anim",    "battle_anim_script.inc",  "src/battle_anim.c",             "sScriptCmdTable",               0x00, 0x2F),
    ("battle_ai",      "battle_ai_script.inc",    "src/battle_ai_script_commands.c", "sBattleAICmdTable",            0x00, 0x62),
    ("contest_ai",     "contest_ai_script.inc",   "src/contest_ai.c",              "sContestAICmdTable",            0x00, 0x87),
    ("field_effect",   "field_effect_script.inc", "src/field_effect.c",            "gFieldEffectScriptFuncs",       0x00, 0x07),
]

ROLE_BY_DIRECTIVE = {
    ".byte":  "b8",
    ".short": "u16",
    ".2byte": "u16",
    ".hword": "u16",
    ".int":   "u32",
    ".word":  "u32",
    ".4byte": "u32",
    # ".4bye" is a typo in the qualified contest_ai_script.inc
    # (if_most_jamming_move, opcode 0x30, unused in the qualified data);
    # GNU as errors on unknown directives, but the macro is never invoked,
    # so the typo never reached the assembler. We record the intended width.
    ".4bye":  "u32",
}


def parse_macros(path):
    """Parse .macro blocks from a GNU as macro file."""
    text = open(path, "r", encoding="utf-8").read()
    # strip C-style comments
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    macros = []
    pos = 0
    while True:
        m = re.search(r"^\\?\s*\.macro\s+(\w+)(.*)$", text[pos:], re.M)
        if not m:
            break
        start = pos + m.start()
        name = m.group(1)
        argstr = m.group(2).strip()
        # find the matching .endm
        endm = re.search(r"^\\?\s*\.endm\s*$", text[start + m.end() - m.start():], re.M)
        if not endm:
            raise SystemExit(f"{path}: macro {name} has no .endm")
        body = text[start + m.end() - m.start(): start + m.end() - m.start() + endm.start()]
        args = []
        if argstr:
            for a in argstr.split(","):
                a = a.strip()
                a = re.sub(r":req$|:vararg$|:default.*", "", a).strip()
                if a and a not in ("", "\\"):
                    args.append(a.lstrip("\\"))
        emissions = []
        for line in body.splitlines():
            line = line.strip()
            if not line or line.startswith(".if") or line.startswith(".endif") \
               or line.startswith(".else") or line.startswith(".endm") \
               or line.startswith("/*"):
                continue
            mm = re.match(r"^(\.byte|\.short|\.2byte|\.hword|\.int|\.word|\.4byte|\.4bye)(.*)$", line)
            if mm:
                emissions.append((mm.group(1), mm.group(2).strip()))
        macros.append(dict(name=name, args=args, body=body, emissions=emissions))
        pos = start + m.end() - m.start() + endm.end()
    return macros


def literal_u32(s):
    s = s.strip()
    s = re.sub(r"^\\", "", s)
    # numeric literal or constant expression? Only pure literals are opcodes.
    m = re.match(r"^0[xX]([0-9a-fA-F]+)$", s)
    if m:
        return int(m.group(1), 16)
    m = re.match(r"^(\d+)$", s)
    if m:
        return int(m.group(1), 10)
    return None


WIDTH = {".byte": 1, ".short": 2, ".2byte": 2, ".hword": 2,
         ".int": 4, ".word": 4, ".4byte": 4, ".4bye": 4}


def expand_macro(name, args, macros_by_name, stack=None, depth=0):
    """Expand a macro (with argument substitution) into a list of
    (directive, expr) emissions. Wrapper macros invoke other macros; the
    expansion follows them. `.if`/`.else`/`.endif` conditional branches must
    have equal width in each branch (the anim VM's `createsprite`); both
    branches are counted once."""
    if depth > 8:
        raise SystemExit(f"{name}: macro expansion too deep (cycle?)")
    stack = stack or []
    if name in stack:
        raise SystemExit(f"{name}: macro cycle {stack + [name]}")
    if name not in macros_by_name:
        raise SystemExit(f"{name}: unknown macro in expansion")
    mac = macros_by_name[name]
    argmap = {}
    for a, v in zip(mac["args"], args):
        argmap[a] = v
    if len(args) > len(mac["args"]):
        argmap["args"] = args[len(mac["args"]):]
    em = []
    body = mac["body"]
    lines = [ln.strip() for ln in body.splitlines() if ln.strip()]
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith((".if", ".ifdef", ".ifndef")):
            # skip to matching .else/.endif, expand the branch whose lines
            # match the emitted widths; we require both branches equal width.
            depth_ = 1
            j = i + 1
            branch = [[], []]
            cur = 0
            while depth_ > 0 and j < len(lines):
                if lines[j].startswith(".if"):
                    depth_ += 1
                elif lines[j].startswith(".endif"):
                    depth_ -= 1
                    if depth_ == 0:
                        break
                elif lines[j].startswith(".else") and depth_ == 1:
                    cur = 1
                else:
                    branch[cur].append(lines[j])
                j += 1
            if len(branch[0]) != len(branch[1]):
                raise SystemExit(f"{name}: conditional branches differ in shape")
            body2 = "\n".join(branch[0])
            i = j
            lines = lines[:i] + body2.splitlines() + lines[i + 1:] if False else lines
            # expand branch 0 emissions (equal-width rule) and continue
            for ln in branch[0]:
                mm = re.match(r"^(\.byte|\.short|\.2byte|\.hword|\.int|\.word|\.4byte|\.4bye)(.*)$", ln)
                if mm:
                    em.append((mm.group(1), substitute(mm.group(2), argmap)))
                else:
                    m2 = re.match(r"^(\w+)\s*(.*)$", ln)
                    if m2:
                        subargs = split_args(substitute(m2.group(2), argmap))
                        em.extend(expand_macro(m2.group(1), subargs, macros_by_name,
                                               stack + [name], depth + 1))
            continue
        if line.startswith((".endif", ".else", ".endm")):
            i += 1
            continue
        mm = re.match(r"^(\.byte|\.short|\.2byte|\.hword|\.int|\.word|\.4byte|\.4bye)(.*)$", line)
        if mm:
            em.append((mm.group(1), substitute(mm.group(2), argmap)))
            i += 1
            continue
        m2 = re.match(r"^(\w+)\s*(.*)$", line)
        if m2:
            subargs = split_args(substitute(m2.group(2), argmap))
            em.extend(expand_macro(m2.group(1), subargs, macros_by_name,
                                   stack + [name], depth + 1))
            i += 1
            continue
        # non-emission line (comment) — skip
        i += 1
    return em


def substitute(expr, argmap):
    for a, v in argmap.items():
        expr = re.sub(r"\\" + re.escape(a) + r"\b", v, expr)
    return expr


def split_args(s):
    if not s.strip():
        return []
    return [a.strip() for a in s.split(",") if a.strip()]


def build_grammar(fam):
    macro_file, c_file, table_name, op_min, op_max = fam[1], fam[2], fam[3], fam[4], fam[5]
    macros = parse_macros(os.path.join(MACRO_DIR, macro_file))
    macros_by_name = {m["name"]: m for m in macros}
    out = []
    for mac in macros:
        em = expand_macro(mac["name"], [a for a in mac["args"]], macros_by_name)
        if not em:
            continue  # macro with no emissions even after expansion
        opcode = literal_u32(em[0][1])
        if opcode is None:
            # opcode is an expression (e.g. a constant name) — must be resolvable;
            # record it raw for the cross-check to report.
            opcode = "?" + em[0][1]
        operands = []
        size = 0
        for i, (directive, expr) in enumerate(em):
            w = WIDTH[directive]
            if i == 0:
                size += 1
                continue
            # operand expression: extract referenced macro args
            args_used = sorted({a for a in mac["args"] if re.search(r"\\" + a + r"\b", expr)})
            operands.append(dict(name=expr[:40], width=w, args=args_used))
            size += w
        out.append(dict(opcode=opcode, command=mac["name"], size=size,
                        operands=operands, args=mac["args"],
                        emissions=em))
    return out


def extract_c_table(fam):
    """Extract (opcode -> handler name) from the C interpreter table."""
    macro_file, c_file, table_name, op_min, op_max = fam[1], fam[2], fam[3], fam[4], fam[5]
    src = open(os.path.join(REPO, c_file), "r", encoding="utf-8").read()
    m = re.search(r"\b%s\b" % re.escape(table_name), src)
    if not m:
        raise SystemExit(f"{c_file}: table {table_name} not found")
    open_brace = src.find("{", m.end())
    if open_brace < 0:
        raise SystemExit(f"{c_file}: table {table_name} has no opening brace")
    body = src[open_brace + 1:]
    # handlers until the closing };
    entries = []
    pos = 0
    depth = 1
    while depth > 0:
        c = body[pos]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        pos += 1
    table_text = body[:pos - 1]
    for line in table_text.splitlines():
        mm = re.match(r"\s*(\w+)\s*,?\s*(//.*)?$", line)
        if mm:
            entries.append(mm.group(1))
    return entries


def main():
    outdir = os.path.join(REPO, "resources", "extraction", "emerald", "bpee01", "battle")
    os.makedirs(outdir, exist_ok=True)
    report = []
    for family in FAMILIES:
        fname = family[0]
        grams = build_grammar(family)
        handlers = extract_c_table(family)
        op_min, op_max = family[4], family[5]
        # opcode -> encodings
        by_op = {}
        for g in grams:
            if isinstance(g["opcode"], int):
                by_op.setdefault(g["opcode"], []).append(g)
        # table-expected opcodes
        expected = set(range(op_min, op_max + 1))
        emitted = set(by_op.keys())
        missing = sorted(expected - emitted)
        extra = sorted(emitted - expected)
        # handler count sanity
        hlen = len(handlers)
        # alias encodings with different sizes (decode ambiguity risk)
        size_aliases = []
        for op, encs in sorted(by_op.items()):
            sizes = {e["size"] for e in encs}
            if len(sizes) > 1:
                size_aliases.append((op, [(e["command"], e["size"]) for e in encs]))
        report.append((fname, len(grams), hlen, len(emitted), missing, extra, size_aliases))
        # emit TOML
        with open(os.path.join(outdir, f"h1_grammar_{fname}.generated.toml"), "w") as f:
            f.write(f"# Generated by tools/gen3_resources/battle_family/h1_grammar.py\n")
            f.write(f"# Machine-readable grammar for the Emerald {fname} VM.\n")
            f.write(f"grammar_version = 1\nfamily = \"{fname}\"\n")
            f.write(f"table_entries = {hlen}\nopcode_range = [{op_min}, {op_max}]\n\n")
            for g in grams:
                op = g["opcode"]
                if isinstance(op, int):
                    opstr = hex(op)
                else:
                    opstr = op
                f.write(f"[[opcodes]]\nopcode = {opstr}\n")
                f.write(f"command = \"{g['command']}\"\nsize = {g['size']}\n")
                f.write("operands = [\n")
                for o in g["operands"]:
                    name = o["name"].replace("\\", "\\\\")
                    f.write(f'  {{ name = "{name}", width = {o["width"]}, args = {o["args"]} }},\n')
                f.write("]\n\n")
    for r in report:
        print(r)


if __name__ == "__main__":
    main()
