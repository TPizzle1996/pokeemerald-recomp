"""h1_analyze.py — R13-H1 battle-family graph extractor.

Recomputes the exact qualified inventories for the five H families
(battle scripts, battle anim scripts, battle AI, contest AI, field-effect
scripts) from:

  ../pokeemerald-reference/build/emerald/data/*.o   (qualified objects)
  ../pokeemerald-reference/pokeemerald.elf          (qualified linked ELF)
  ../pokeemerald-reference/pokeemerald.gba          (qualified ROM)
  data/*.s                                          (qualified sources)
  pokeemerald-linux64                               (native binary, nm)

Method: a source-level decoder walks each data/*.s file in assembly order
(labels, macro invocations, .align/.int/.short/.byte emissions), producing
exact label offsets and instruction spans. Every label offset is verified
against the qualified object's symbol values and the total against the
object's script_data size. Relocations are then assigned to instruction
operand slots / routing-table rows and classified from (a) the grammar
operand slot, (b) the target symbol's identity (in-file script label,
in-file data span, or external engine symbol whose qualified-ELF section
proves its class).

Outputs (resources/extraction/emerald/bpee01/battle/):
  h1_family_inventory.generated.toml
  h1_relocation_census.generated.toml     (every relocation, exact)
  h1_engine_bindings.generated.toml       (per-symbol semantic bindings)
  h1_interior_target_proof.generated.toml
  h1_modules.generated.toml               (future H2 module ownership)
  h1_routing_rows.generated.toml
  h1_census_closure.generated.toml        (included + excluded, exact)
  h1_state_v5_surfaces.generated.toml
  h1_oracle.generated.toml                (three-way static oracle)

--check: regenerate in a temp dir and require byte-identical output.

Analysis/generation only; nothing is linked into the runtime.
"""

import json
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h1_elf import ObjectFile, LinkedElf  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
REF = os.path.join(os.path.dirname(REPO), "pokeemerald-reference")
OUTDIR = os.path.join(REPO, "resources", "extraction", "emerald", "bpee01", "battle")
ROM_PATH = os.path.join(REF, "pokeemerald.gba")
ELF_PATH = os.path.join(REF, "pokeemerald.elf")
NATIVE = os.path.join(REPO, "pokeemerald-linux64")
GEN3_GBA_ROM_BASE = 0x08000000

FAMILY_OBJECTS = [
    ("battle_scripts_1", "battle"),
    ("battle_scripts_2", "battle"),
    ("battle_anim_scripts", "battle_anim"),
    ("battle_ai_scripts", "battle_ai"),
    ("contest_ai_scripts", "contest_ai"),
    ("field_effect_scripts", "field_effect"),
]

ROUTING_TABLES = {
    "battle_scripts_1": ["gBattleScriptsForMoveEffects"],
    "battle_scripts_2": ["gBattlescriptsForBallThrow", "gBattlescriptsForUsingItem",
                         "gBattlescriptsForRunningByItem", "gBattlescriptsForSafariActions"],
    "battle_anim_scripts": ["gMovesWithQuietBGM", "gBattleAnims_Moves",
                            "gBattleAnims_StatusConditions", "gBattleAnims_General",
                            "gBattleAnims_Special"],
    "battle_ai_scripts": ["gBattleAI_ScriptsTable"],
    "contest_ai_scripts": ["gContestAI_ScriptsTable"],
    "field_effect_scripts": ["gFieldEffectScriptPointers"],
}

# anim vararg commands: (macro, named-arg count) — the trailing args are
# .short argv entries; the binary embeds the count in a byte.
ANIM_VARARG = {
    "createsprite": 3,         # template, anim_battler, subpriority_offset
    "createvisualtask": 2,     # addr, priority
    "createsoundtask": 1,      # addr
}

# byte-free helper macros invoked in the qualified data files (defined in
# asm/macros.inc; they emit only .equ/.set, no bytes)
NO_BYTES_MACROS = {"enum_start", "enum"}


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def split_args_smart(s):
    """Split an asm argument list on commas that are not inside parentheses.

    Anim scripts pass function-style expressions in vararg slots
    (e.g. `createsprite ..., 0, RGB(18, 31, 12)`); a naive split counts the
    inner commas as extra args and inflates the vararg count by 2 each.
    """
    parts = []
    depth = 0
    cur = []
    for ch in s:
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth = max(0, depth - 1)
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def load_grammar(family):
    import tomllib
    with open(os.path.join(OUTDIR, f"h1_grammar_{family}.generated.toml"), "rb") as f:
        g = tomllib.load(f)
    by_op = {}
    by_name = {}
    for op in g["opcodes"]:
        by_op.setdefault(op["opcode"], []).append(op)
        by_name[op["command"]] = op
    return by_op, by_name


def native_symbols():
    out = subprocess.run(["nm", "-S", "--size-sort", NATIVE],
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and re.match(r"^[0-9a-fA-F]+$", parts[0]):
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
            typ = parts[2]
            name = parts[3] if len(parts) > 3 else ""
            if name:
                syms[name] = dict(addr=addr, size=size, typ=typ)
    return syms


def rom_u32(rom, gba_addr):
    return struct.unpack_from("<I", rom, gba_addr - GEN3_GBA_ROM_BASE)[0]


# ---------------------------------------------------------------------------
# source-level decoder
# ---------------------------------------------------------------------------

def decode_source(src_path, family, grammar_by_name):
    """Walk data/<file>.s in assembly order.

    Returns dict(labels=[{name, offset, is_global}], spans=[...], total).
    A span is the byte region [label.offset, next_label.offset) with
    emissions recorded: ('instr', command, size) or ('data', width, n).

    Conditional assembly (.ifdef/.ifndef/.else/.endif) is evaluated with the
    qualified build's symbol set: the reference Makefile passes only
    `--defsym MODERN=0` (MODERN is not referenced in these files), so BUGFIX
    and every other name are undefined.
    """
    lines = open(src_path, "r", encoding="utf-8").read().splitlines()
    pos = 0
    labels = []
    events = []  # (pos, kind, payload)
    macro_re = re.compile(r"^\t(\w+)\s*(.*)$")
    DEFINED = {"MODERN"}
    cond = []  # stack of active-until-else? booleans
    for lineno, line in enumerate(lines, 1):
        line_stripped = line.strip()
        if not line_stripped or line_stripped.startswith("/*") or line_stripped.startswith("*"):
            continue
        # conditional directives — evaluate and maybe skip
        mc = re.match(r"^\.(ifdef|ifndef)\s+(\w+)(\s*@.*)?$", line_stripped)
        if mc:
            sym = mc.group(2)
            truth = sym in DEFINED if mc.group(1) == "ifdef" else sym not in DEFINED
            cond.append(truth)
            continue
        if line_stripped.startswith(".else"):
            cond[-1] = not cond[-1]
            continue
        if line_stripped.startswith(".endif"):
            cond.pop()
            continue
        if not all(cond):
            continue  # in an inactive branch — no bytes, no labels
        # label? (GNU as allows whitespace between the name and the colon:
        # `HydroPumpHitSplats\t:` appears in the qualified anim source)
        m = re.match(r"^([A-Za-z_.$][A-Za-z0-9_.$]*)\s*(::?)\s*(\s*@.*)?$", line_stripped)
        if m:
            labels.append(dict(name=m.group(1), offset=pos, is_global=(m.group(2) == "::"),
                               lineno=lineno))
            continue
        m = re.match(r"^\.align\s+(\d+)$", line_stripped)
        if m:
            al = int(m.group(1))
            if al > 1:
                pos = (pos + (1 << al) - 1) // (1 << al) * (1 << al)
            continue
        m = re.match(r"^(\.int|\.short|\.2byte|\.hword|\.byte|\.word|\.4byte|\.4bye)\s+(.*)$",
                     line_stripped)
        if m:
            directive = m.group(1)
            w = {".int": 4, ".word": 4, ".4byte": 4, ".4bye": 4,
                 ".short": 2, ".2byte": 2, ".hword": 2, ".byte": 1}[directive]
            # strip trailing @ comment BEFORE splitting: comments may contain
            # commas (e.g. `.4byte Move_COUNT @ cannot be reached, because ...`)
            argtext = re.sub(r"\s*@.*$", "", m.group(2))
            args = split_args_smart(argtext)
            n = max(1, len(args))
            events.append((pos, "data", dict(width=w, count=n, lineno=lineno)))
            pos += w * n
            continue
        m = macro_re.match(line)
        if m:
            name = m.group(1)
            argstr = re.sub(r"\s*@.*$", "", m.group(2)).strip()
            argc = len(split_args_smart(argstr))
            gram = grammar_by_name.get(name)
            if gram is None:
                if name in NO_BYTES_MACROS:
                    continue
                fail(f"{src_path}:{lineno}: unknown macro {name!r}")
            size = gram["size"]
            if family == "battle_anim" and name in ANIM_VARARG:
                named = ANIM_VARARG[name]
                varargs = max(0, argc - named)
                # the grammar's operands include the trailing `.2byte \argv`
                # vararg slot; exclude it from the fixed-size base
                base = 1 + sum(o["width"] for o in gram["operands"]
                               if o["name"].strip() != "argv")
                size = base + 2 * varargs
            events.append((pos, "instr", dict(command=name, size=size, argc=argc,
                                              lineno=lineno)))
            pos += size
            continue
        # anything else (comments, .include, .section, .set, .equ) — no bytes
    return dict(labels=labels, events=events, total=pos, src=src_path)


def build_spans(decoded):
    labels = sorted(decoded["labels"], key=lambda l: l["offset"])
    events = sorted(decoded["events"], key=lambda e: e[0])
    spans = []  # {start, end, label, is_global, emissions}
    for i, lab in enumerate(labels):
        end = labels[i + 1]["offset"] if i + 1 < len(labels) else decoded["total"]
        span_events = [e for e in events if lab["offset"] <= e[0] < end]
        spans.append(dict(start=lab["offset"], end=end, label=lab["name"],
                          is_global=lab["is_global"], events=span_events,
                          lineno=lab["lineno"]))
    return spans


# ---------------------------------------------------------------------------
# per-family analysis
# ---------------------------------------------------------------------------

def analyze_family(oname, family, grammar_by_op, grammar_by_name, elf, rom, nsyms, shnames):
    obj_path = os.path.join(REF, "build", "emerald", "data", oname + ".o")
    obj = ObjectFile(obj_path)
    sd = obj.section("script_data")
    if sd is None:
        fail(f"{oname}: no script_data")
    data = obj.section_bytes("script_data")
    size = len(data)

    # symbols
    sd_idx = obj.section_index("script_data")
    syms = obj.symbols_in(sd_idx)
    sym_by_value = {}
    for s in syms:
        sym_by_value.setdefault(s["value"], []).append(s)
    # relocations
    rels = obj.relocs_for("script_data")
    rel_by_offset = {}
    for r in rels:
        if r["offset"] in rel_by_offset:
            fail(f"{oname}: duplicate reloc at {hex(r['offset'])}")
        if r["type"] != 2:
            fail(f"{oname}: non-ABS32 reloc type {r['type']} at {hex(r['offset'])}")
        rel_by_offset[r["offset"]] = r
    # linked base
    base = None
    for s in sorted(syms, key=lambda x: x["value"]):
        if s["binding"] == 1:
            addr = elf.global_addr(s["name"])
            if addr is not None:
                base = addr - s["value"]
                break
    if base is None:
        fail(f"{oname}: cannot compute linked base")

    # source decode + verify — decode the QUALIFIED reference source (the
    # recomp repo's data/*.s are a newer pret revision with renamed labels;
    # only the reference files produce the qualified objects' symtabs).
    src_path = os.path.join(REF, "data", oname + ".s")
    decoded = decode_source(src_path, family, grammar_by_name)
    if decoded["total"] != size:
        fail(f"{oname}: source total {decoded['total']} != object size {size}")
    for lab in decoded["labels"]:
        if lab["offset"] not in sym_by_value:
            fail(f"{oname}: label {lab['name']}@{hex(lab['offset'])} missing from symtab")
        names = [s["name"] for s in sym_by_value[lab["offset"]]]
        if lab["name"] not in names:
            fail(f"{oname}: label {lab['name']}@{hex(lab['offset'])} name mismatch {names[:4]}")
    for v, ss in sym_by_value.items():
        if not any(lab["offset"] == v for lab in decoded["labels"]):
            # symbol at a value with no source label? possible alias of an
            # unlabeled spot — flag only if nonzero
            if v != 0:
                fail(f"{oname}: symtab value {hex(v)} has no source label "
                     f"({[s['name'] for s in ss][:2]})")

    spans = build_spans(decoded)
    # classify spans: routing / script / data
    for sp in spans:
        if sp["label"] in ROUTING_TABLES.get(oname, []):
            sp["kind"] = "routing"
        else:
            kinds = [e[1] for e in sp["events"]]
            sp["kind"] = "script" if kinds and all(k == "instr" for k in kinds) else \
                         ("data" if kinds else "empty")

    # instructions per span (span-relative absolute offsets)
    instructions = []
    for sp in spans:
        if sp["kind"] != "script":
            continue
        p = sp["start"]
        for (epos, ekind, payload) in sp["events"]:
            if ekind != "instr":
                fail(f"{oname}: script span {sp['label']} contains non-instr at {hex(epos)}")
            if epos != p:
                fail(f"{oname}: instruction gap in {sp['label']} at {hex(p)}")
            gram = grammar_by_name[payload["command"]]
            # fixed operand slots (exclude the anim vararg `.2byte \argv` slot:
            # its size varies per invocation and its entries never carry relocs)
            slots = []
            off = 1
            for o in gram["operands"]:
                if family == "battle_anim" and o["name"].strip() == "argv":
                    continue
                slots.append(dict(offset=off, width=o["width"], name=o["name"]))
                off += o["width"]
            instructions.append(dict(offset=epos, command=payload["command"],
                                     size=payload["size"], argc=payload["argc"],
                                     span_label=sp["label"], span_start=sp["start"],
                                     operand_slots=slots))
            p += payload["size"]
        if p != sp["end"]:
            fail(f"{oname}: script span {sp['label']} size mismatch {p} vs {sp['end']}")

    # table rows
    table_rows = []
    for sp in spans:
        if sp["kind"] != "routing":
            continue
        width = 2 if sp["label"] == "gMovesWithQuietBGM" else 4
        n = (sp["end"] - sp["start"]) // width
        for j in range(n):
            off = sp["start"] + j * width
            stored = struct.unpack_from("<I", data, off)[0] if width == 4 else \
                     struct.unpack_from("<H", data, off)[0]
            table_rows.append(dict(offset=off, stored=stored, table=sp["label"],
                                   row=j, width=width,
                                   target=symbol_target(obj, rel_by_offset.get(off),
                                                        stored, base, size)))

    # data rows (AI type lists etc.)
    data_rows = []
    for sp in spans:
        if sp["kind"] != "data":
            continue
        p = sp["start"]
        rows = []
        for (epos, ekind, payload) in sp["events"]:
            w = payload["width"]
            for k in range(payload["count"]):
                off = p + k * w
                val = struct.unpack_from("<I", data, off)[0] if w == 4 else \
                      (struct.unpack_from("<H", data, off)[0] if w == 2 else data[off])
                rows.append(dict(offset=off, width=w, stored=val,
                                 target=symbol_target(obj, rel_by_offset.get(off),
                                                      val, base, size)))
            p += payload["count"] * w
        if p != sp["end"]:
            fail(f"{oname}: data span {sp['label']} size mismatch")
        data_rows.append(dict(span=sp["label"], start=sp["start"], end=sp["end"],
                              rows=rows))

    return dict(name=oname, family=family, size=size, base=base, data=data,
                syms=syms, sym_by_value=sym_by_value, rel_by_offset=rel_by_offset,
                spans=spans, instructions=instructions, table_rows=table_rows,
                data_rows=data_rows, obj=obj, sd_idx=sd_idx)


def symbol_target(obj, reloc, stored, base, size):
    """Target description of a relocation (or None).

    R_ARM_ABS32 (REL object): final = S + A where A is the value stored at
    the relocation site. The qualified objects were assembled by GNU as with
    the addend carried in the stored word (0 for a plain named-label
    reference — the target is the symbol's own value).

    infile targets: sym is a script_data symbol (a `::`/local label or the
    section symbol, value 0). The section-relative target offset is
    value + stored in every case.
    external targets: sym is UND; stored is the addend A.
    """
    if reloc is None:
        return None
    sym = obj.symbols[reloc["symidx"]]
    sd_idx = obj.section_index("script_data")
    if sym["shndx"] == sd_idx:
        return dict(kind="infile", symbol=sym["name"], value=sym["value"],
                    addend=stored, target_offset=sym["value"] + stored)
    return dict(kind="external", symbol=sym["name"], addend=stored)


# ---------------------------------------------------------------------------
# classification
# ---------------------------------------------------------------------------

# Access width of each grammar command's ENGINE-target pointer operands,
# from the qualified handler semantics (T2_READ_PTR -> typed dereference).
# Macros that EXPAND to byte-handlers have byte access even though their
# operand slot is u32:
#   setword -> 4x setbyte, copyword -> 4x copybyte,
#   setstatchanger/setmoveeffect/moveend* -> setbyte,
#   jumpifmovehadnoeffect -> jumpifbyte gMoveResultFlags.
# An ENGINE-target reloc under a command absent from this map is a STOP
# condition (unexplained access width).
ACCESS_WIDTH = {
    # u8 handlers
    "setbyte": 1, "addbyte": 1, "subbyte": 1, "orbyte": 1, "bicbyte": 1,
    "copybyte": 1, "copyarray": 1, "copyarraywithindex": 1,
    "jumpifbyte": 1, "jumpifbyteequal": 1, "jumpifbytenotequal": 1,
    # macros expanding to byte handlers
    "setword": 1, "copyword": 1, "setstatchanger": 1, "setmoveeffect": 1,
    "moveendto": 1, "moveendfrom": 1, "moveendfromto": 1, "moveendall": 1,
    "moveendcase": 1, "jumpifmovehadnoeffect": 1,
    # u16 handlers
    "sethword": 2, "copyhword": 2, "jumpifhalfword": 2,
    # u32 handlers / pointer reads
    "orword": 4, "bicword": 4, "jumpifword": 4, "jumpifbattletype": 4,
    "jumpifnotbattletype": 4, "jumpifmove": 4, "jumpifnotmove": 4,
    "playanimation": 4, "playanimation_var": 4,
    "printfromtable": 4, "printselectionstringfromtable": 4,
    "createsprite": 4, "createvisualtask": 4, "createsoundtask": 4,
    "field_eff_callnative": 4, "field_eff_loadfadedpal_callnative": 4,
    "field_eff_loadfadedpal": 4, "field_eff_loadpal": 4,
}


def access_width_of(command, oname, offset):
    w = ACCESS_WIDTH.get(command)
    if w is None:
        fail(f"{oname}: ENGINE-target command {command}@{hex(offset)} has "
             f"no explained access width")
    return w


def find_containing(res, offset):
    """Span and (for script spans) instruction containing an offset."""
    for sp in res["spans"]:
        if sp["start"] <= offset < sp["end"]:
            return sp
    return None


def classify_infile_target(res, target, operand_width):
    """SCRIPT_TARGET classification for an in-family relocation target."""
    off = target["target_offset"]
    names = res["sym_by_value"].get(off, [])
    if names:
        return ("SCRIPT_TARGET",
                dict(target=names[0]["name"], offset=off, root=True, addend=target["addend"]))
    # interior offset — find the containing span (must be script kind)
    sp = find_containing(res, off)
    if sp is None:
        fail(f"{res['name']}: interior target {hex(off)} outside all spans")
    if sp["kind"] != "script":
        fail(f"{res['name']}: interior target {hex(off)} inside non-script span "
             f"{sp['label']} (kind={sp['kind']}) — pointer into a table")
    return ("SCRIPT_TARGET",
            dict(target=sp["label"], offset=off, root=False, addend=target["addend"],
                 inside=sp["label"]))


def classify_external_target(res, target, elf, secnames, nsyms):
    """Engine class for an external relocation target, proven by the
    qualified ELF section of the resolved symbol (with name corroboration
    and the anonymous-binding gate)."""
    sym = target["symbol"]
    addend = target["addend"]
    # anonymous raw address binding gate: no empty-name externals
    if sym == "":
        fail(f"{res['name']}: anonymous external binding addend={hex(addend)}")
    defined = [s for s in elf.by_name.get(sym, []) if s["shndx"] != 0]
    if not defined:
        fail(f"{res['name']}: external {sym} unresolved in qualified ELF")
    s = defined[0]
    sec = secnames.get(s["shndx"], "?")
    gba = s["value"]
    final = gba + addend
    size = s["size"] if s["size"] > 0 else None
    if size is None:
        # fall back to the native symbol's size for bounds
        nm = nsyms.get(sym)
        if nm:
            size = nm["size"]
    native = nsyms.get(sym)
    native_addr = native["addr"] if native else None
    if sec == "ewram":
        return ("ENGINE_EWRAM_TARGET",
                dict(symbol=sym, gba_base=gba, addend=addend, final_gba=final,
                     size=size, native_addr=native_addr, section=sec))
    if sec == ".text":
        return ("ENGINE_CALLBACK",
                dict(symbol=sym, gba_base=gba, addend=addend, final_gba=final,
                     size=size, native_addr=native_addr, section=sec))
    if sec == ".rodata":
        if "SpriteTemplate" in sym:
            cls = "ENGINE_SPRITE_TEMPLATE_TARGET"
        elif sym.startswith("gSpritePalette") or "Tilemap" in sym \
                or "BgImage" in sym or "Image" in sym or "Gfx" in sym:
            cls = "ENGINE_GFX_TARGET"
        else:
            cls = "ENGINE_TABLE_TARGET"
        return (cls,
                dict(symbol=sym, gba_base=gba, addend=addend, final_gba=final,
                     size=size, native_addr=native_addr, section=sec))
    if sec == "script_data":
        fail(f"{res['name']}: CROSS-FAMILY external {sym} -> script_data "
             f"({hex(final)}) — direct cross-family bytecode pointer")
    fail(f"{res['name']}: external {sym} in unclassified ELF section {sec}")


def build_reloc_records(results, elf, secnames, nsyms):
    """Classify every relocation across the six objects.

    Returns per-object lists of records, plus per-family class tallies and
    the nonzero-addend audit records. Any relocation that cannot be placed
    in an operand slot / table row / data row is a STOP condition, and any
    external target whose final address lands inside one of the six H
    arenas is a CROSS-FAMILY BYTECODE POINTER (brief section 14) — STOP.
    """
    all_records = {}
    tallies = {}
    addends = []  # nonzero-addend audit records
    null_literals = {}  # u32 pointer slots with stored 0 and no reloc (NULL)
    arenas = [(r["name"], r["base"], r["base"] + r["size"])
              for r in results.values()]
    for oname, res in results.items():
        fam = res["family"]
        # instruction lookup by offset
        instr_at = {}
        for ins in res["instructions"]:
            instr_at[ins["offset"]] = ins
        rows_at = {r["offset"]: r for r in res["table_rows"]}
        drow_at = {}
        for dspan in res["data_rows"]:
            for r in dspan["rows"]:
                drow_at[r["offset"]] = r
        records = []
        t = {}
        for off in sorted(res["rel_by_offset"]):
            rel = res["rel_by_offset"][off]
            stored = struct.unpack_from("<I", res["data"], off)[0]
            target = symbol_target(res["obj"], rel, stored, res["base"], res["size"])
            sp = find_containing(res, off)
            if sp is None:
                fail(f"{oname}: reloc at {hex(off)} outside all spans")
            if sp["kind"] == "script":
                ins = instr_at.get(off)
                if ins is None:
                    # find instruction containing off
                    for ioff in sorted(instr_at):
                        i = instr_at[ioff]
                        if ioff <= off < ioff + i["size"]:
                            ins = i
                            break
                if ins is None:
                    fail(f"{oname}: reloc at {hex(off)} not in any instruction")
                slot = None
                for s in ins["operand_slots"]:
                    if ins["offset"] + s["offset"] == off:
                        slot = s
                        break
                if slot is None:
                    fail(f"{oname}: reloc at {hex(off)} not at an operand slot "
                         f"of {ins['command']}@{hex(ins['offset'])}")
                operand = f"{ins['command']}.op{slot['name'][:24]}" \
                          if slot["name"] else ins["command"]
                if target["kind"] == "infile":
                    cls, detail = classify_infile_target(res, target, slot["width"])
                    final_gba = res["base"] + detail["offset"]
                    tgt_sym = detail["target"]
                    reloc_sym = target["symbol"]
                else:
                    cls, detail = classify_external_target(res, target, elf,
                                                           secnames, nsyms)
                    final_gba = detail["final_gba"]
                    tgt_sym = reloc_sym = detail["symbol"]
                rec = dict(offset=off, gba_addr=res["base"] + off, stored=stored,
                           final_gba=final_gba, span=sp["label"],
                           command=ins["command"], operand=operand,
                           width=slot["width"], target_class=cls,
                           addend=detail.get("addend", 0), target_symbol=tgt_sym,
                           reloc_symbol=reloc_sym,
                           target_offset=detail.get("offset", detail.get("final_gba")),
                           root=detail.get("root"),
                           access_width=access_width_of(ins["command"], oname, off)
                           if cls.startswith("ENGINE_") else None)
            elif sp["kind"] == "routing":
                row = rows_at.get(off)
                if row is None:
                    fail(f"{oname}: reloc at {hex(off)} in routing span "
                         f"{sp['label']} but not at a row start")
                if target["kind"] != "infile":
                    fail(f"{oname}: routing row {row['table']}[{row['row']}] "
                         f"has external target {target['symbol']}")
                cls, detail = classify_infile_target(res, target, 4)
                rec = dict(offset=off, gba_addr=res["base"] + off, stored=stored,
                           final_gba=res["base"] + detail["offset"],
                           span=sp["label"],
                           command=f"row:{row['table']}[{row['row']}]",
                           operand=f"row:{row['table']}[{row['row']}]", width=4,
                           target_class=cls, addend=detail["addend"],
                           target_symbol=detail["target"],
                           reloc_symbol=target["symbol"],
                           target_offset=detail["offset"],
                           root=detail["root"])
            elif sp["kind"] == "data":
                row = drow_at.get(off)
                if row is None:
                    fail(f"{oname}: reloc at {hex(off)} in data span "
                         f"{sp['label']} but not at a row start")
                if target["kind"] != "infile":
                    fail(f"{oname}: data row {sp['label']} has external target "
                         f"{target['symbol']}")
                cls, detail = classify_infile_target(res, target, row["width"])
                rec = dict(offset=off, gba_addr=res["base"] + off, stored=stored,
                           final_gba=res["base"] + detail["offset"],
                           span=sp["label"], command=f"data:{sp['label']}",
                           operand=f"data:{sp['label']}", width=row["width"],
                           target_class=cls, addend=detail["addend"],
                           target_symbol=detail["target"],
                           reloc_symbol=target["symbol"],
                           target_offset=detail["offset"],
                           root=detail["root"])
            else:
                fail(f"{oname}: reloc at {hex(off)} in empty span {sp['label']}")
            records.append(rec)
            t[cls] = t.get(cls, 0) + 1
            if rec["addend"] != 0:
                addends.append(dict(family=fam, offset=off, gba=rec["gba_addr"],
                                    symbol=target["symbol"], addend=rec["addend"],
                                    cls=cls, access_width=rec.get("access_width")))
            if target["kind"] == "external":
                # cross-family gate: an external target's final address must
                # not land inside any of the six H arenas
                for (aname, abase, aend) in arenas:
                    if abase <= rec["final_gba"] < aend:
                        fail(f"{oname}: CROSS-FAMILY external {target['symbol']} "
                             f"-> {hex(rec['final_gba'])} inside arena {aname} "
                             f"({hex(abase)}..{hex(aend)})")
        all_records[oname] = records
        tallies[oname] = t
        # NULL-literal pointer slots: u32 operand slots whose stored value is
        # 0 and that carry no relocation (literal 0 constants, e.g. the
        # `tryfaintmon` NULL or `playanimation` arg=NULL slots). These never
        # appear in the relocation census by construction.
        null_literals[oname] = 0
        for ins in res["instructions"]:
            for s in ins["operand_slots"]:
                if s["width"] != 4:
                    continue
                off = ins["offset"] + s["offset"]
                if off in res["rel_by_offset"]:
                    continue
                stored = struct.unpack_from("<I", res["data"], off)[0]
                if stored == 0:
                    null_literals[oname] += 1
    return all_records, tallies, addends, null_literals


def build_bindings(all_records, elf, secnames, nsyms):
    """Per-symbol engine-binding census (unique by symbol+addend)."""
    bind = {}
    for oname, recs in all_records.items():
        for r in recs:
            if not r["target_class"].startswith("ENGINE_"):
                continue
            key = (r["target_symbol"], r["addend"])
            d = bind.setdefault(key, dict(
                gba_base_symbol=r["target_symbol"], addend=r["addend"],
                encoded_gba_value=0, native_binding_symbol=r["target_symbol"],
                allowed_offset=0, width_or_type=None, mutability="?",
                semantic_family=None, occurrences=0))
            d["occurrences"] += 1
            if d["encoded_gba_value"] == 0:
                defined = [s for s in elf.by_name.get(r["target_symbol"], [])
                           if s["shndx"] != 0]
                if not defined:
                    fail(f"binding {r['target_symbol']} unresolved in ELF")
                s = defined[0]
                secname = secnames.get(s["shndx"], "?")
                d["encoded_gba_value"] = s["value"]
                d["allowed_offset"] = s["size"] if s["size"] > 0 else \
                    nsyms.get(r["target_symbol"], {}).get("size", 0)
                d["mutability"] = "const" if secname == ".rodata" else "mutable"
                d["section"] = secname
            widths = {r["width"]}
            d["width_or_type"] = sorted(widths)
            if r["access_width"] is not None:
                d.setdefault("access_widths", set()).add(r["access_width"])
            if d["semantic_family"] is None:
                d["semantic_family"] = r["target_class"]
    for b in bind.values():
        b["access_widths"] = sorted(b.get("access_widths", set()))
    return sorted(bind.values(), key=lambda b: (b["gba_base_symbol"], b["addend"]))


def elf_u32(elf, gba_addr):
    """Read the final linked value from the qualified ELF image at a GBA
    address (sh_addr -> file offset via sh_offset)."""
    for s in elf.sections:
        if s["sh_addr"] and s["sh_type"] != 8 \
                and s["sh_addr"] <= gba_addr < s["sh_addr"] + s["sh_size"]:
            return struct.unpack_from("<I", elf.data,
                                      s["sh_offset"] + (gba_addr - s["sh_addr"]))[0]
    fail(f"ELF: no PROGBITS section contains {hex(gba_addr)}")


def build_oracle(all_records, results, rom, elf):
    """Three-way static oracle: ROM raw == ELF image == generated record.
    For every relocation site, rom_u32(gba_addr) must equal the ELF image
    value and both must equal the record's final target (base + offset for
    in-family targets, symbol value + addend for engine targets)."""
    rows = []
    mismatches = []
    for oname, recs in all_records.items():
        res = results[oname]
        for r in recs:
            romv = rom_u32(rom, r["gba_addr"])
            elfv = elf_u32(elf, r["gba_addr"])
            ok = romv == elfv == r["final_gba"]
            if not ok:
                mismatches.append(dict(family=res["family"], offset=r["offset"],
                                       gba=hex(r["gba_addr"]), rom=hex(romv),
                                       elf=hex(elfv), expected=hex(r["final_gba"]),
                                       target_symbol=r["target_symbol"],
                                       target_class=r["target_class"]))
            rows.append(dict(family=res["family"], offset=r["offset"],
                             gba=hex(r["gba_addr"]), rom=hex(romv),
                             elf=hex(elfv), expected=hex(r["final_gba"]), ok=ok))
    return rows, mismatches


# ---------------------------------------------------------------------------
# module + proof tables
# ---------------------------------------------------------------------------

def module_key(family, label):
    """emerald:<family-key>/<dash-normalised root>"""
    prefixes = {
        "battle": ("battle-script", "BattleScript_"),
        "battle_anim": ("battle-anim-script", ""),
        "battle_ai": ("battle-ai", "AI_"),
        "contest_ai": ("contest-ai", "AI_"),
        "field_effect": ("field-effect-script", "gFieldEffectScript_"),
    }
    ns, prefix = prefixes[family]
    root = label[len(prefix):] if label.startswith(prefix) else label
    root = re.sub(r"([a-z0-9])([A-Z])", r"\1-\2", root).lower()
    return f"emerald:{ns}/{root}"


def build_modules(results):
    mods = {}
    for oname, res in results.items():
        fam = res["family"]
        by_off = {}
        for sp in res["spans"]:
            by_off.setdefault(sp["start"], []).append(sp)
        spans_by_start = {sp["start"]: sp for sp in res["spans"]}
        entries = []
        for sp in sorted(res["spans"], key=lambda s: s["start"]):
            key = module_key(fam, sp["label"])
            # aliases: other spans starting at the same offset
            aliases = [x["label"] for x in by_off.get(sp["start"], [])
                       if x["label"] != sp["label"]]
            entries.append(dict(
                key=key, family=fam, object=res["name"], span_label=sp["label"],
                start=sp["start"], end=sp["end"], kind=sp["kind"],
                byte_count=sp["end"] - sp["start"],
                exports=[sp["label"]] + aliases, aliases=aliases,
                gba_base=res["base"] + sp["start"]))
        mods[oname] = entries
    return mods


def build_interior_proof(all_records, results):
    proof = {}
    for oname, recs in all_records.items():
        fam = results[oname]["family"]
        script_ops = [r for r in recs if r["target_class"] == "SCRIPT_TARGET"]
        roots = [r for r in script_ops if r["root"]]
        interior = [r for r in script_ops if not r["root"]]
        distinct_roots = sorted({r["target_symbol"] for r in roots})
        distinct_all = sorted({r["target_symbol"] for r in script_ops})
        # fan-in for every distinct target
        fan = {}
        for r in script_ops:
            fan[r["target_symbol"]] = fan.get(r["target_symbol"], 0) + 1
        top = sorted(fan.items(), key=lambda kv: (-kv[1], kv[0]))[:12]
        proof[oname] = dict(family=fam, script_operands=len(script_ops),
                            root_targets=len(roots), interior_targets=len(interior),
                            distinct_targets=len(distinct_all),
                            distinct_root_targets=len(distinct_roots),
                            fan_in_top=[dict(target=t, fan_in=n) for t, n in top])
    return proof


def build_closure(all_records, tallies, results):
    total = 0
    per_family = {}
    for oname in results:
        recs = all_records[oname]
        n = len(recs)
        total += n
        per_family[oname] = dict(relocs=n, classified=n, unresolved=0,
                                 ambiguous=0, unknown=0,
                                 classes=dict(sorted(tallies[oname].items())))
    return dict(universe=total, included=total, excluded=0, remainder=0,
                unresolved=0, ambiguous=0, unknown=0, per_family=per_family)


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------

def tstr(s):
    """Escape a string for TOML basic-string emission (symbol names and
    grammar operand expressions may contain backslashes, e.g. `\\ptr`)."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def emit_toml(path, text):
    with open(path, "w") as f:
        f.write(text)


def main():
    check = "--check" in sys.argv
    outdir = OUTDIR if not check else tempfile.mkdtemp(prefix="h1check-")
    os.makedirs(outdir, exist_ok=True)
    rom = open(ROM_PATH, "rb").read()
    elf = LinkedElf(ELF_PATH)
    secnames = {i: s["name"] for i, s in enumerate(elf.sections)}
    nsyms = native_symbols()

    grammars = {f: load_grammar(f) for f in
                ["battle", "battle_anim", "battle_ai", "contest_ai", "field_effect"]}

    results = {}
    for (oname, family) in FAMILY_OBJECTS:
        by_op, by_name = grammars[family]
        res = analyze_family(oname, family, by_op, by_name, elf, rom, nsyms, secnames)
        results[oname] = res
        print(f"{oname}: bytes={res['size']} base=0x{res['base']:08x} "
              f"spans={len(res['spans'])} instr={len(res['instructions'])} "
              f"rows={len(res['table_rows'])} data_spans={len(res['data_rows'])} "
              f"relocs={len(res['rel_by_offset'])}")

    all_records, tallies, addend_audit, null_literals = \
        build_reloc_records(results, elf, secnames, nsyms)
    bindings = build_bindings(all_records, elf, secnames, nsyms)
    oracle_rows, oracle_bad = build_oracle(all_records, results, rom, elf)
    modules = build_modules(results)
    interior = build_interior_proof(all_records, results)
    closure = build_closure(all_records, tallies, results)

    # ---- family inventory
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Exact qualified family inventories (objects, bases, spans, relocs).",
             "inventory_version = 1\n"]
    lines.append("[summary]")
    lines.append(f"object_bytes = {sum(r['size'] for r in results.values())}")
    lines.append(f"relocs = {closure['universe']}\n")
    for oname in results:
        r = results[oname]
        lines.append(f"[[families]]")
        lines.append(f'name = "{r["family"]}"')
        lines.append(f'object = "{oname}.o"')
        lines.append(f"base = {hex(r['base'])}")
        lines.append(f"size = {r['size']}")
        lines.append(f"spans = {len(r['spans'])}")
        lines.append(f"instructions = {len(r['instructions'])}")
        lines.append(f"routing_rows = {len(r['table_rows'])}")
        lines.append(f"data_spans = {len(r['data_rows'])}")
        lines.append(f"relocs = {len(r['rel_by_offset'])}")
    # five family arenas: the hull [min base, max end) per family — these
    # are the 5 State-v5 ranges H registers (plan section 18). Battle spans
    # two objects with the field-effect arena between them, so the battle
    # hull includes that gap; per-object ranges stay exact in the modules
    # file.
    hulls = {}
    for (oname, family) in FAMILY_OBJECTS:
        r = results[oname]
        h = hulls.setdefault(family, [r["base"], r["base"] + r["size"]])
        h[0] = min(h[0], r["base"])
        h[1] = max(h[1], r["base"] + r["size"])
    lines.append("\n[arenas]")
    for fam in sorted(hulls):
        s, e = hulls[fam]
        lines.append(f'"{fam}" = [{hex(s)}, {hex(e)}]')
        lines.append(f"{fam}_hull_bytes = {e - s}")
    emit_toml(os.path.join(outdir, "h1_family_inventory.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- relocation census
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Every relocation in the six qualified objects, classified.",
             "census_version = 1\n"]
    for oname in results:
        lines.append(f"[[object]]")
        lines.append(f'name = "{oname}"')
        lines.append(f'family = "{results[oname]["family"]}"')
        lines.append(f"base = {hex(results[oname]['base'])}")
        for r in all_records[oname]:
            lines.append("[[object.relocs]]")
            lines.append(f"offset = {r['offset']}")
            lines.append(f"gba_addr = {hex(r['gba_addr'])}")
            lines.append(f"stored = {hex(r['stored'])}")
            lines.append(f'target_class = "{tstr(r["target_class"])}"')
            lines.append(f"addend = {r['addend']}")
            lines.append(f'target_symbol = "{tstr(r["target_symbol"])}"')
            lines.append(f'reloc_symbol = "{tstr(r["reloc_symbol"])}"')
            lines.append(f'target_offset = {hex(r["target_offset"]) if isinstance(r["target_offset"], int) else r["target_offset"]}')
            lines.append(f'final_gba = {hex(r["final_gba"])}')
            lines.append(f'root = {str(bool(r["root"])).lower()}')
            lines.append(f'command = "{tstr(r["command"])}"')
            lines.append(f'operand = "{tstr(r["operand"])}"')
            lines.append(f'width = {r["width"]}')
            if r.get("access_width") is not None:
                lines.append(f"access_width = {r['access_width']}")
            lines.append(f'span = "{tstr(r["span"])}"')
    emit_toml(os.path.join(outdir, "h1_relocation_census.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- engine bindings
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Per-symbol engine-binding census (battle EWRAM fields, u16 ID tables,",
             "# sprite templates, anim callbacks, field-effect gfx/functions).",
             "# gba_base_symbol + addend -> native_binding_symbol + allowed_offset.",
             "bindings_version = 1\n"]
    for b in bindings:
        lines.append("[[bindings]]")
        lines.append(f'gba_base_symbol = "{b["gba_base_symbol"]}"')
        lines.append(f"encoded_gba_value = {hex(b['encoded_gba_value'])}")
        lines.append(f"addend = {b['addend']}")
        lines.append(f'native_binding_symbol = "{b["native_binding_symbol"]}"')
        lines.append(f"allowed_offset = {b['allowed_offset']}")
        lines.append(f"width_or_type = {b['width_or_type']}")
        lines.append(f"access_widths = {b['access_widths']}")
        lines.append(f'mutability = "{b["mutability"]}"')
        lines.append(f'semantic_family = "{b["semantic_family"]}"')
        lines.append(f"occurrences = {b['occurrences']}")
        lines.append(f'section = "{b.get("section", "?")}"')
    emit_toml(os.path.join(outdir, "h1_engine_bindings.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- routing rows
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# The 13 compiled routing tables inside the H arenas, row by row.",
             "# Pointer rows carry a target; gMovesWithQuietBGM rows are u16",
             "# move IDs and carry a value.",
             "routing_version = 1\n"]
    for oname in results:
        for row in results[oname]["table_rows"]:
            tgt = row["target"]
            lines.append("[[rows]]")
            lines.append(f'table = "{row["table"]}"')
            lines.append(f'row = {row["row"]}')
            lines.append(f'width = {row["width"]}')
            lines.append(f'offset = {row["offset"]}')
            lines.append(f"stored = {hex(row['stored'])}")
            lines.append(f'gba_addr = {hex(results[oname]["base"] + row["offset"])}')
            if tgt is None:
                # value row (gMovesWithQuietBGM u16 move IDs — no reloc)
                lines.append("kind = \"value\"")
                lines.append(f"value = {row['stored']}")
            else:
                lines.append("kind = \"pointer\"")
                lines.append(f'target_symbol = "{tgt["symbol"]}"')
                lines.append(f'target_offset = {tgt.get("target_offset", tgt.get("value", 0))}')
    emit_toml(os.path.join(outdir, "h1_routing_rows.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- interior target proof
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Root vs interior SCRIPT_TARGET operands per family.",
             "proof_version = 1\n"]
    for oname in results:
        p = interior[oname]
        lines.append(f"[[family]]")
        lines.append(f'name = "{oname}"')
        lines.append(f'family = "{p["family"]}"')
        lines.append(f"script_operands = {p['script_operands']}")
        lines.append(f"root_targets = {p['root_targets']}")
        lines.append(f"interior_targets = {p['interior_targets']}")
        lines.append(f"distinct_targets = {p['distinct_targets']}")
        lines.append(f"distinct_root_targets = {p['distinct_root_targets']}")
        for t in p["fan_in_top"]:
            lines.append(f"[[family.fan_in]]")
            lines.append(f'target = "{t["target"]}"')
            lines.append(f"fan_in = {t['fan_in']}")
    emit_toml(os.path.join(outdir, "h1_interior_target_proof.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- modules
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Per-root modules (future H2 resources).",
             "modules_version = 1\n"]
    for oname in results:
        for m in modules[oname]:
            lines.append("[[modules]]")
            lines.append(f'key = "{m["key"]}"')
            lines.append(f'family = "{m["family"]}"')
            lines.append(f'object = "{m["object"]}"')
            lines.append(f'span_label = "{m["span_label"]}"')
            lines.append(f"start = {m['start']}")
            lines.append(f"end = {m['end']}")
            lines.append(f"byte_count = {m['byte_count']}")
            lines.append(f'kind = "{m["kind"]}"')
            lines.append(f"gba_base = {hex(m['gba_base'])}")
            lines.append(f"exports = {m['exports']}")
            lines.append(f"aliases = {m['aliases']}")
    emit_toml(os.path.join(outdir, "h1_modules.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- census closure
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# included + excluded = complete relocation universe; zero remainder.",
             "closure_version = 1\n"]
    for k, v in closure.items():
        if k == "per_family":
            continue
        lines.append(f"{k} = {v}")
    lines.append("[per_family]")
    for oname, v in closure["per_family"].items():
        lines.append(f"[per_family.{oname}]")
        for k2, v2 in v.items():
            if k2 == "classes":
                continue
            lines.append(f"{k2} = {v2}")
        lines.append(f"[per_family.{oname}.classes]")
        for cls, n in v["classes"].items():
            lines.append(f"{cls} = {n}")
    emit_toml(os.path.join(outdir, "h1_census_closure.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- oracle
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Three-way static oracle: ROM raw == object stored for every reloc.",
             f"oracle_version = 1\nchecked = {len(oracle_rows)}",
             f"mismatches = {len(oracle_bad)}\n"]
    if oracle_bad:
        lines.append("[[mismatches]]")
        for m in oracle_bad:
            lines.append(f'family = "{m["family"]}"')
            lines.append(f"offset = {m['offset']}")
            lines.append(f'gba = "{m["gba"]}"')
            lines.append(f'rom = "{m["rom"]}"')
            lines.append(f'elf = "{m["elf"]}"')
            lines.append(f'expected = "{m["expected"]}"')
            lines.append(f'target_symbol = "{m["target_symbol"]}"')
            lines.append(f'target_class = "{m["target_class"]}"')
    emit_toml(os.path.join(outdir, "h1_oracle.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- state-v5 surfaces
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# State-v5 capture surfaces for the H VMs (plan section 14, verified).",
             "state_version = 1\n"]
    surfaces = [
        ("gBattlescriptCurrInstr", 1, "EWRAM",
         "H key + instruction offset (INSTRUCTION_START)"),
        ("battleScriptsStack.ptr[]", 8, "GAME_BSS",
         "H key + next-instruction offset (IP+5 returns)"),
        ("battleCallbackStack.function[]", 8, "GAME_BSS",
         "engine function image (unchanged)"),
        ("gSelectionBattleScripts[]/gPalaceSelectionBattleScripts[]", 8, "EWRAM",
         "H key + offset"),
        ("gAIScriptPtr", 1, "EWRAM", "H key + offset (quiescent stale IP)"),
        ("AI_ScriptsStack.ptr[]", 8, "GAME_BSS", "H key + offset (size 0 at VBlank)"),
        ("sBattleAnimScriptPtr/sBattleAnimScriptRetAddr", 2, "EWRAM",
         "H key + offset"),
        ("gAnimScriptCallback", 1, "EWRAM", "engine function (existing special-case)"),
        ("sTrainerBattleEndScript + A/B returns", 3, "EWRAM",
         "existing G slots, class widened to H targets"),
    ]
    total_slots = sum(s for (_, s, _, _) in surfaces)
    # scalars FIRST (a key after [[surfaces]] would bind to the last element)
    lines.append(f"total_slots = {total_slots}")
    lines.append("worst_case_quiescent_records = 19")
    lines.append("worst_case_mid_battle_records = 27")
    lines.append("sidecar_cap = 4096")
    lines.append("arena_ranges = 5")
    lines.append("live_ranges_post_h = 6382")
    lines.append("range_cap = 8192")
    # 20,988 entries today (post-G) + exactly one pack entry per H module
    # (plan §18 estimated ≈23,049 on ≈2,056 modules; H1's exact count is
    # 2,089 modules including the 13 routing spans and 8 empty alignment spans)
    lines.append(f"pack_entries_post_h = {20988 + sum(len(v) for v in modules.values())}")
    lines.append("pack_entry_cap = 32768")
    for name, slots, sl, form in surfaces:
        lines.append("[[surfaces]]")
        lines.append(f'name = "{name}"')
        lines.append(f"slots = {slots}")
        lines.append(f'slice = "{sl}"')
        lines.append(f'stable_form = "{form}"')
    emit_toml(os.path.join(outdir, "h1_state_v5_surfaces.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- nonzero-addend audit (STOP gate on unexplained)
    audit = sorted(addend_audit, key=lambda a: (a["family"], a["offset"]))
    classified = []
    for a in audit:
        # kind from the handler's access width: byte field / halfword field /
        # struct member (u32 slot) / array element or table entry (rodata
        # arrays) / section-relative script target. A nonzero addend on a
        # SCRIPT_TARGET reloc is the section-relative OFFSET OF THE ROOT LABEL
        # ITSELF (anim/AI objects relocate against the section symbol; the
        # proof's interior-target count is 0, so no addend points into the
        # middle of a span).
        if a["cls"] == "SCRIPT_TARGET":
            kind = "section-relative-target"
        elif a["access_width"] == 1:
            kind = "byte-field"
        elif a["access_width"] == 2:
            kind = "halfword-field"
        elif a["cls"] in ("ENGINE_TABLE_TARGET", "ENGINE_GFX_TARGET",
                          "ENGINE_SPRITE_TEMPLATE_TARGET"):
            kind = "array-element/table-entry"
        else:
            kind = "struct-member"
        classified.append(dict(family=a["family"], offset=a["offset"],
                               gba=hex(a["gba"]), symbol=a["symbol"],
                               addend=a["addend"], cls=a["cls"], kind=kind,
                               access_width=a["access_width"]))
    # bounds validation: addend + access width must fit the symbol's size
    bounds_ok = True
    for b in bindings:
        if b["addend"] == 0 or b["allowed_offset"] == 0:
            continue
        if b["addend"] + max(b["access_widths"]) > b["allowed_offset"]:
            bounds_ok = False
            print(f"BOUNDS FAIL: {b['gba_base_symbol']}+{b['addend']} access "
                  f"{b['access_widths']} > size {b['allowed_offset']}")
    lines = ["# Generated by tools/gen3_resources/battle_family/h1_analyze.py",
             "# Every relocation with a nonzero addend, classified and bounds-checked.",
             f"addend_version = 1\ncount = {len(classified)}",
             f"all_bounds_ok = {str(bounds_ok).lower()}\n"]
    for a in classified:
        lines.append("[[addends]]")
        lines.append(f'family = "{a["family"]}"')
        lines.append(f"offset = {a['offset']}")
        lines.append(f'gba = "{a["gba"]}"')
        lines.append(f'symbol = "{a["symbol"]}"')
        lines.append(f"addend = {a['addend']}")
        lines.append(f'class = "{a["cls"]}"')
        lines.append(f'kind = "{a["kind"]}"')
        if a["access_width"] is not None:
            lines.append(f"access_width = {a['access_width']}")
    emit_toml(os.path.join(outdir, "h1_addend_audit.generated.toml"),
              "\n".join(lines) + "\n")

    # ---- summary prints
    print()
    print("== class tallies ==")
    for oname in results:
        print(f"{oname}: {dict(sorted(tallies[oname].items()))}")
    print()
    print(f"bindings: {len(bindings)}  oracle: {len(oracle_rows)} checked, "
          f"{len(oracle_bad)} bad")
    print(f"nonzero addends: {len(classified)}  bounds_ok: {bounds_ok}")
    print(f"NULL-literal pointer slots: "
          f"{dict(sorted(null_literals.items()))}  "
          f"total={sum(null_literals.values())}")
    print(f"universe: {closure['universe']} included: {closure['included']} "
          f"excluded: {closure['excluded']} remainder: {closure['remainder']}")
    for oname in results:
        p = interior[oname]
        print(f"{oname}: script_ops={p['script_operands']} "
              f"root={p['root_targets']} interior={p['interior_targets']} "
              f"distinct={p['distinct_targets']}")

    if check:
        # byte-compare every emitted file with the committed outputs
        bad = []
        for fn in os.listdir(outdir):
            with open(os.path.join(outdir, fn), "rb") as f:
                got = f.read()
            committed = os.path.join(OUTDIR, fn)
            if not os.path.exists(committed):
                bad.append(f"{fn}: MISSING in committed outputs")
                continue
            with open(committed, "rb") as f:
                want = f.read()
            if got != want:
                bad.append(f"{fn}: differs")
        if bad:
            for b in bad:
                print(f"--check FAIL: {b}")
            sys.exit(1)
        print(f"--check OK: {len(os.listdir(outdir))} files byte-identical")

    with open(os.path.join(outdir, "_h1_debug.json"), "w") as f:
        json_out = {}
        for n, r in results.items():
            json_out[n] = dict(
                size=r["size"], base=r["base"],
                spans=[dict(start=s["start"], end=s["end"], label=s["label"],
                            kind=s["kind"]) for s in r["spans"]],
                instructions=[dict(offset=i["offset"], command=i["command"],
                                   size=i["size"]) for i in r["instructions"]],
                relocs=[dict(offset=o, sym=rel["sym"]) for o, rel in
                        sorted(r["rel_by_offset"].items())])
        json.dump(json_out, f, indent=1)


if __name__ == "__main__":
    main()
