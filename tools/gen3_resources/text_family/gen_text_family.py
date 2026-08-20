#!/usr/bin/env python3
"""R13-C: text string family generator.

Derives the emerald:text/* family from the qualified pret reference ELF +
retail ROM and emits resources/extraction/emerald/bpee01/text/:

  * inventory/catalog/bindings/ownership/consumers .generated.toml
  * labels/<family>/<canonical>.bin        - 4,824 per-label C-side artifacts
  * bundles/<bundle-key>.bin               - 363 constructed bundle blobs
    (header: u32 labelCount, u32 dataOffset, u32 offsets[labelCount], then
     the canonical bytes of every label in canonical-name order - each
     label's bytes start at offsets[i]; the blob is self-describing and
     deterministic)
  * include/emerald/resources/text_native.generated.h  + the definition
    src/emerald/resources/text_native_table.generated.c (5,187-entry seam
    inventory)
  * include/emerald/resources/text_slots.generated.h + text_slots_table.
    generated.c (pointer slots for the slot families)
  * include/emerald/resources/text_skeletons.generated.h (skeleton-table
    row metadata)

Scope (the R13-C brief + docs/R13C_AUDIT_CORRECTIONS.md, authoritative):
  * 12,777 source-defined text labels / 903,151 canonical bytes
    (12,756 + 21 rename-completed labels per AC-3; canonical = exact Gen
     III charmap bytes to the first 0xFF terminator, bounded 4 KiB;
     validated against the pret charmap escape grammar with 0 failures).
  * Script side: 7,933 labels / 749,265 B in script_data (asm-source total
    7,953 / 751,947 B incl. the 20 gift_*.inc labels that resolve in
    .rodata). Published as 363 bundle resources
    (304 maps + 35 data/text + 24 data/scripts incl. data/event_scripts.s
    - AC-2 correction), COMPILED_PENDING_MIGRATION (additive; script
    operands embed native addresses - R13-G re-emits them).
  * C side: 4,824 per-label resources (4,821 plan-scope + 3 rename labels)
    across 11 families; ROM_BASE_ONLY for the live cutover families
    (battle/move/ability/nature/shared/system/match-call/ribbon),
    COMPILED_PENDING_MIGRATION for the table-blocked families
    (item/pokedex/easy-chat).
  * 478 duplicate-byte groups / 1,168 symbols kept (no dedup, plan §4);
    19 empty, 50 single-char; 0 same-address aliases.
  * 97 dual-referenced asm labels (script operands + direct C code),
    documented per-symbol, all in deferred families.
  * 13 out-of-contract labels (recomp-only, absent from the qualified ROM):
    sText_EmptyStatus + 12 settings-UI strings - verified absent, stay
    compiled.

Keys follow the plan §2 vocabulary with the brief's per-label override:
  emerald:text/<family>/<canonical-symbol>     (lowercase, '_' -> '-')
  emerald:text/map/<MapName>, emerald:text/data/<file>, emerald:text/scripts/<file>
  for the bundles. All key names pass Gen3ResourceId_ValidateCanonicalName
  (a-z 0-9 . _ -, no uppercase); the symbol-form collision check is a hard
  gate.

Every pinned count is enforced as a hard gate; --check must be a no-op
diff. The pack pipeline (gen3-elf-manifest with the bundle binding
passthrough, gen3-pack-build with the artifact-file payload path) re-proves
the artifacts; bundle records carry source_artifact instead of rom_offset.

Usage:
  gen_text_family.py [root] [elf] [rom] [outdir] [--check]
    root   repo root (default .)
    elf    qualified pret ELF (default ../pokeemerald-reference/pokeemerald.elf)
    rom    retail-matching ROM (default ../pokeemerald-reference/pokeemerald.gba)
    outdir resource extraction root (default resources/extraction/emerald/bpee01)
"""

import argparse
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

GEN3_GBA_ROM_BASE = 0x08000000
ROM_SIZE = 0x1000000
ROM_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"

# ------------------------------------------------------------- hard pins
# (docs/R13C_AUDIT_CORRECTIONS.md §7 final + §8 + §9, authoritative)
PINNED_TOTAL_LABELS = 12777          # 12,756 + 21 rename-completed (AC-3)
PINNED_TOTAL_BYTES = 903157          # 900,887 + 2,264 (AC-3) + 6 (#112)
PINNED_SCRIPT_LABELS = 7933          # script_data section (7,913 + 20 AC-3)
PINNED_SCRIPT_BYTES = 749265         # 747,007 + 2,258
PINNED_ASM_LABELS = 7953             # asm-source incl. 20 gift in .rodata
PINNED_ASM_BYTES = 751947            # 749,265 + 2,682
PINNED_GIFT_LABELS = 20
PINNED_GIFT_BYTES = 2682
PINNED_C_SCOPE_LABELS = 4821         # plan-scope, excl. renames/gift/c-misc
PINNED_C_SCOPE_BYTES = 151129        # 151,117 + 6 (gEasyChatWord_WeRe) + 6 (#112)
PINNED_RENAME_LABELS = 24            # 3 (AC-1) + 21 (AC-3)
PINNED_RENAME_BYTES = 2345           # 81 + 2,264
PINNED_C_RESOURCES = 4824            # per-label resources (4,821 + 3 renames)
PINNED_C_RESOURCE_BYTES = 151210     # 151,123 + 81 + 6 (#112)
PINNED_CMISC_LABELS = 2              # gSpeciesNames + gTrainerClassNames
PINNED_CMISC_BYTES = 22
PINNED_EMPTY = 19                    # 1 B (terminator only)
PINNED_SINGLE = 50                   # 2 B
PINNED_ALIAS_GROUPS = 0
PINNED_DUP_GROUPS = 478              # 476 + "None$"×4 + 1 (AC-3, measured)
PINNED_DUP_SYMBOLS = 1168
PINNED_REDUNDANT_BYTES = 23891
PINNED_BUNDLE_MAPS = 304
PINNED_BUNDLE_DATA_TEXT = 35
PINNED_BUNDLE_SCRIPTS = 24   # 23 data/scripts + data/event_scripts.s (AC-2)
PINNED_BUNDLES = 363         # corrected 362 -> 363 (docs/R13C_AUDIT_CORRECTIONS.md §8)
PINNED_DUAL_REF = 97
PINNED_SCAN_BOUND = 4096             # canonical scan bound (never hit: max 824)
PINNED_KEY_COLLISION_GROUPS = {"geasychatword-were": ["gEasyChatWord_WeRe",
                                                      "gEasyChatWord_Were"]}

# Per-family (labels, bytes) - AC-1 §2 + AC-3 §9c corrections (map-dialogue,
# misc-scripts, tv, easy-chat +21; system -2 double-count). All other rows
# EXACT.
FAMILY_PINS = {
    "map-dialogue": (4361, 428130), "trainer": (1142, 67095),
    "match-call": (629, 60934), "pokedex": (387, 58017), "tv": (401, 55507),
    "apprentice": (288, 49891), "misc-scripts": (512, 37484),
    "shared": (1655, 29915), "system": (613, 28790), "move": (355, 17251),
    "item": (310, 15101), "battle": (522, 11989), "frontier": (180, 11597),
    "cable-club": (91, 7713), "easy-chat": (1008, 7101), "berry": (41, 4305),
    "pokemon-news": (12, 3950), "misc": (27, 1714), "ability": (78, 1859),
    "pokedex-rating": (25, 1780), "ribbon": (66, 1265),
    "frontier-brain": (28, 882), "mauville-man": (18, 638),
    "nature": (25, 162), "mart": (3, 87),
}

# Recomp source label -> qualified reference ELF label (upstream rename).
# Rows 1-3: AC-1 §4. Rows 4+: AC-3 (2026-08-18) - labels the newer pret
# revision renamed whose bytes ARE in the qualified ROM under the
# reference names (each mapped pair verified byte/content-identical).
RENAME_MAP = {
    "gText_Judgment": "gText_Judgement",
    "sText_GotchaPkmnCaughtPlayer": "sText_GotchaPkmnCaught",
    "sText_GotchaPkmnCaughtWally": "sText_GotchaPkmnCaught2",
    # AC-3: map/script renames (text labels misnamed *EventScript_ in the
    # older revision; content-identical).
    "BattleFrontier_BattleTowerLobby_Text_DirectYouToBattleRoom":
        "BattleFrontier_BattleTowerLobby_EventScript_DirectYouToBattleRoom",
    "OldaleTown_Text_TownSign": "OldaleTown_Text_CitySign",
    "MauvilleCity_PokemonCenter_1F_Text_HaveYouHeardOfWord":
        "MauvilleCity_PokemonCenter_1F_Text_HaveYouHeardOfPhrase",
    "Route110_TrickHousePuzzle7_Text_WroteSecretCodeLockOpened":
        "Route110_TrickHousePuzzle7_EventScript_WroteSecretCodeLockOpened",
    "Route110_TrickHousePuzzle8_Text_WroteSecretCodeLockOpened":
        "Route110_TrickHousePuzzle8_EventScript_WroteSecretCodeLockOpened",
    # AC-3: easy-chat word rename ("WE'RE", 6 B).
    "gEasyChatWord_WeRe": "gEasyChatWord_WeAre",
    # AC-3: trade.h sMessages -> sTradeMessages (older pret revision name;
    # the reference symbol exists only in trade.o's locals at 0x0832debc,
    # 36 B, rows byte-identical). Without the rename, discovery fell back
    # to the flat table's first `sMessages` (berry_crush.o).
    "sMessages": "sTradeMessages",
    # AC-3: tv.inc BravoTrainer renames -> gTVBravoTrainerBattleTowerTextNN.
    # Text-matched (11) + order-matched identical "None$" bytes (4: None1-4
    # -> Text07-10, source order identical in both revisions).
    "BravoTrainerBattleTower_Text_Intro": "gTVBravoTrainerBattleTowerText00",
    "BravoTrainerBattleTower_Text_NewRecord": "gTVBravoTrainerBattleTowerText01",
    "BravoTrainerBattleTower_Text_Lost": "gTVBravoTrainerBattleTowerText02",
    "BravoTrainerBattleTower_Text_Won": "gTVBravoTrainerBattleTowerText03",
    "BravoTrainerBattleTower_Text_LostFinal": "gTVBravoTrainerBattleTowerText04",
    "BravoTrainerBattleTower_Text_Satisfied": "gTVBravoTrainerBattleTowerText05",
    "BravoTrainerBattleTower_Text_Unsatisfied": "gTVBravoTrainerBattleTowerText06",
    "BravoTrainerBattleTower_Text_None1": "gTVBravoTrainerBattleTowerText07",
    "BravoTrainerBattleTower_Text_None2": "gTVBravoTrainerBattleTowerText08",
    "BravoTrainerBattleTower_Text_None3": "gTVBravoTrainerBattleTowerText09",
    "BravoTrainerBattleTower_Text_None4": "gTVBravoTrainerBattleTowerText10",
    "BravoTrainerBattleTower_Text_Response": "gTVBravoTrainerBattleTowerText11",
    "BravoTrainerBattleTower_Text_ResponseSatisfied": "gTVBravoTrainerBattleTowerText12",
    "BravoTrainerBattleTower_Text_ResponseUnsatisfied": "gTVBravoTrainerBattleTowerText13",
    "BravoTrainerBattleTower_Text_Outro": "gTVBravoTrainerBattleTowerText14",
}

# Recomp-only labels with no retail ROM presence: verified ABSENT from the
# qualified ELF, stay compiled, out of the migration contract (AC-1 §4).
OUT_OF_CONTRACT = [
    ("sText_EmptyStatus", "src/battle_message.c",
     "recomp settings-UI placeholder; byte pattern absent from the "
     "qualified ROM"),
    ("gText_VSync", "src/strings.c", "recomp-only PC options UI string"),
    ("gText_Volume", "src/strings.c", "recomp-only PC options UI string"),
    ("gText_WindowScale", "src/strings.c", "recomp-only PC options UI string"),
    ("gText_Back", "src/strings.c", "recomp-only PC options UI string"),
    ("gText_BorderBackground", "src/strings.c",
     "recomp-only PC options UI string"),
    ("gText_BorderBackgroundName", "src/strings.c",
     "recomp-only PC options UI string"),
    ("gText_BorderBackgroundOff", "src/strings.c",
     "recomp-only PC options UI string"),
    ("gText_BorderFrame", "src/strings.c", "recomp-only PC options UI string"),
    ("gText_DisplaySettings", "src/strings.c",
     "recomp-only PC options UI string"),
    ("gText_Fullscreen", "src/strings.c", "recomp-only PC options UI string"),
    ("gText_IntegerScale", "src/strings.c",
     "recomp-only PC options UI string"),
    ("gText_PkmnFainted_FldPsn", "src/strings.c",
     "recomp-only PC options UI string"),
]

# Plan-family -> key segment (plan §2 vocabulary; item/move/ability/nature/
# pokedex use per-label symbol-form keys per the brief's per-label model).
KEY_SEGMENT = {
    "battle": "battle", "move": "move", "ability": "ability",
    "nature": "nature", "item": "item", "pokedex": "pokedex",
    "easy-chat": "easychat", "match-call": "matchcall", "ribbon": "ribbon",
    "shared": "system", "system": "system",
}

# Live cutover families (plan §11 tranches 1-2): ROM_BASE_ONLY after the
# R13-C cutover (skeleton tables for battle/move/ability/nature, pointer
# slots for shared/system/match-call/ribbon). The item-description family
# flipped ROM_BASE_ONLY at R13-D2 (gItems[].description re-points into the
# item arena) via a targeted ownership flip - it stays OUT of LIVE_FAMILIES
# so the slot/skeleton/table emission is unchanged. Table-blocked families
# (item-lookups, pokedex/easy-chat) stay COMPILED_PENDING_MIGRATION.
LIVE_FAMILIES = frozenset(
    {"battle", "move", "ability", "nature", "shared", "system",
     "match-call", "ribbon"})

# R13-C §9/§10: comprehensive text-pointer table discovery + host-array
# generation. Every `const u8 *const NAME[...]` / `const struct T NAME[...]`
# / `const struct T *const NAME[...]` DEFINITION in the native C scope
# (src/*.c, src/data/text/*.h, src/data/contest_text_tables.h,
# src/data/script_menu.h) is classified against the qualified ELF:
#   - a 4-byte table word equal to a known text-label address makes the
#     table live text (host array + per-word fills); its labels must be
#     family-consistent (all LIVE -> cut over; all deferred -> stays
#     compiled; mixed -> fail);
#   - a word equal to another discovered text table's address makes the
#     row a sub-table pointer (kind-2 fill);
#   - NULL words are NULL rows (stay NULL); anything else (constants,
#     non-text pointers) is not a text table and stays compiled.
# Fail-closed: the ELF layout is authoritative (designated indices and
# multi-label lines resolve through the compiled words, never by parsing
# source rows); the source rows' known-label tokens must equal each ELF
# row's label words (multiset); row counts must agree; a struct table's
# constants must be re-derivable from the ELF row bytes.

# 16 family arenas (plan §9, authoritative): every label (C-side +
# bundle-local) belongs to exactly one; the seam packs each arena's labels
# in canonical-name order (16-aligned payload zone, contiguous labels) and
# the per-arena {labelCount, byteTotal} are pinned below and validated at
# publish (fail-closed, §8). Key order is the arena index order.
ARENA_KEYS = [
    "battle", "move", "ability", "nature", "item", "pokedex",
    "system-shared", "tv", "matchcall", "apprentice", "ribbon",
    "frontier", "frontier-brain", "easy-chat", "berry", "misc",
]
C_ARENA_OF_FAMILY = {
    "battle": "battle", "move": "move", "ability": "ability",
    "nature": "nature", "item": "item", "pokedex": "pokedex",
    "easy-chat": "easy-chat", "shared": "system-shared",
    "system": "system-shared", "match-call": "matchcall",
    "ribbon": "ribbon",
}
ASM_ARENA_OF_FAMILY = {
    "tv": "tv", "apprentice": "apprentice", "match-call": "matchcall",
    "frontier": "frontier", "frontier-brain": "frontier-brain",
    "berry": "berry", "system": "system-shared",
}


def arena_of(record):
    """Arena key for a record (label, family, path, kind, section, off,
    size). C labels by family; asm labels by family (contest_link folds
    into system via asm_family; everything unmapped -> misc)."""
    _, fam, _, kind, _, _, _ = record
    if kind == "c":
        return C_ARENA_OF_FAMILY[fam]
    return ASM_ARENA_OF_FAMILY.get(fam, "misc")


# Struct types that can appear as text-table row types (R13-C cutover).
# The field model below parses the definition from the table's own file
# or the header it lives in / moves to; the generated host arrays .c
# includes the header for every struct type used.
STRUCT_HEADER = {
    "MenuAction": "menu.h",
    "ListMenuItem": "list_menu.h",
    "EasyChatScreenTemplate": "easy_chat.h",
    "MatchCallText": "match_call.h",
    "MultiTrainerMatchCallText": "match_call.h",
    "StorageMessage": "pokemon_storage_system.h",
    "MessageWindowInfo": "union_room_chat.h",
    "SearchOptionText": "pokedex.h",
    "SearchMenuTopBarItem": "pokedex.h",
    "SearchMenuItem": "pokedex.h",
    # R13-C: the struct is defined in pokenav_match_call_data.c (which has
    # no header of its own); its interface decls and the CHECK_PAGE_ENTRY_COUNT
    # enum both live in pokenav.h, so that is the header the generated host
    # arrays include for it.
    "MatchCallCheckPageOverride": "pokenav.h",
    "WindowTemplate": "window.h",
    # R13-C discovery-gap struct types (all move to headers in the
    # cutover source edit): the party/box main-menu rows, the frontier
    # pass landmarks, the naming screen templates, and the match-call
    # header/script types. match_call_text_data_t is the typedef name —
    # compiled-constant rows reference it through the extern path.
    "MainMenuText": "pokemon_storage_system.h",
    "MapLandmark": "frontier_pass.h",
    "NamingScreenTemplate": "naming_screen.h",
    "MatchCallTextDataStruct": "match_call.h",
    "match_call_text_data_t": "match_call.h",
    "MatchCallStructNPC": "match_call.h",
    "MatchCallStructTrainer": "match_call.h",
    "MatchCallBirch": "match_call.h",
    "MatchCallRival": "match_call.h",
    "MatchCallWally": "match_call.h",
    "MatchCallLocationOverride": "match_call.h",
}
SCALAR_SIZE = {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4,
               "bool8": 1}

UNION_MEMBER_RE = re.compile(
    r"(?s)(void|u8|bool8|s8|u16|s32)\s*\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)\s*;")
UNION_RE = re.compile(r"union\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
ENUM_BLOCK_RE = re.compile(r"enum\s*\{([^}]*)\}", re.S)
FIELD_RE = re.compile(
    r"(?s)(?:const\s+)?"
    r"(u8\s*\*|void\s*\(\s*\*\s*\w+\s*\)\s*\([^)]*\)"
    r"|u8\s*\(\s*\*\s*\w+\s*\)\s*\([^)]*\)"
    r"|bool8\s*\(\s*\*\s*\w+\s*\)\s*\([^)]*\)"
    r"|(?:struct\s+)?[A-Za-z_]\w*\s*\*"
    r"|u8|s8|u16|s16|u32|s32|bool8)"
    r"\s*(\w+)\s*(\[[^\]]*\])?(?::\s*(\d+))?\s*;")


def resolve_enum_const(text, name):
    """Value of `name` inside an `enum { ... }` block in `text`, or None.
    Handles `A, B = N, C` forms; comment-stripped input expected."""
    for m in ENUM_BLOCK_RE.finditer(text):
        values = {}
        n = 0
        for tok in re.split(r"[\s,]+", m.group(1)):
            tok = tok.strip()
            if not tok:
                continue
            lhs = tok
            if "=" in tok:
                lhs, rhs = (x.strip() for x in tok.split("=", 1))
                if re.match(r"^\d+$", rhs):
                    n = int(rhs)
                elif rhs in values:
                    n = values[rhs]
            values[lhs] = n
            n += 1
        if name in values:
            return values[name]
    return None


def parse_struct_fields(text, sname, enum_sources=()):
    """Field model of `struct sname { ... };` in `text`. Returns
    (raw, layout, gba_size) where raw = [(kind, name, count, bits, extra)]
    and layout = [(kind, name, count, bits, gba_off, gba_size, extra,
    bitpos)]. Raises ValueError on shapes outside the R13-C model
    (fail-closed: the seam re-derives constants from these exact
    layouts); returns None when the definition is absent."""
    # Simple numeric `#define NAME N` resolution for array bounds (e.g.
    # MatchCallText.stringVarFuncIds[NUM_STRVARS_IN_MSG]); enum-constant
    # bounds (e.g. MatchCallCheckPageOverride.flavorTexts[
    # CHECK_PAGE_ENTRY_COUNT]) resolve from `enum_sources` (the TU, its
    # STRUCT_HEADER, and the TU's direct includes). Anything not
    # resolvable stays fail-closed.
    defines = {}
    for dm in re.finditer(r"(?m)^\s*#\s*define\s+(\w+)\s+(\d+)\s*$",
                          text):
        defines[dm.group(1)] = int(dm.group(2))

    def _bound(inner, what):
        if re.match(r"^\d+$", inner):
            return int(inner)
        if inner in defines:
            return defines[inner]
        for src in enum_sources:
            v = resolve_enum_const(src, name=inner)
            if v is not None:
                return v
        raise ValueError(
            f"struct '{sname}': {what} has a non-numeric bound "
            f"'{inner}'")

    # The close may carry a typedef alias (`} match_call_text_data_t;`),
    # so `}` alone is not the boundary — accept an optional identifier
    # between the close brace and the semicolon. The earlier non-greedy
    # `} \w*;` pattern terminated at the first `} name;` — a nested
    # union close (`} func;` in struct MenuAction) — truncating the
    # body. Scan braces instead and end at the struct's own matching
    # close (callers pass comment-stripped text, as _close_brace
    # requires).
    m = re.search(r"struct\s+" + re.escape(sname) + r"\s*\{", text)
    if m is None:
        return None
    close = _close_brace(text, m.end() - 1)
    if close is None:
        raise ValueError(f"struct '{sname}': no matching close brace")
    body = text[m.end():close]
    unions = {}

    def _strip_union(um):
        members = []
        for mm in UNION_MEMBER_RE.finditer(um.group(1)):
            members.append((mm.group(1), mm.group(2), mm.group(3)))
        if not members:
            raise ValueError(
                f"struct '{sname}': union has no function-pointer members")
        unions[um.group(2)] = members
        return "u8 __union__%s;" % um.group(2)

    body = UNION_RE.sub(_strip_union, body)
    raw = []
    for fm in FIELD_RE.finditer(body):
        typ = fm.group(1).strip()
        fname = fm.group(2)
        bracket = fm.group(3)
        bits = int(fm.group(4)) if fm.group(4) else 0
        if fname.startswith("__union__"):
            fname = fname[len("__union__"):]
            raw.append(("func", fname, len(unions[fname]), 0,
                        unions[fname]))
            continue
        if typ.endswith("*"):
            n = 1
            if bracket:
                n = _bound(bracket[1:-1].strip(),
                           f"ptr-array '{fname}'")
            raw.append(("ptrs" if n > 1 else "ptr", fname, n, 0, None))
            continue
        if typ in SCALAR_SIZE:
            if bracket:
                n = _bound(bracket[1:-1].strip(),
                           f"scalar-array '{fname}'")
                raw.append(("scalars", fname, n, 0, typ))
            elif bits:
                raw.append(("bit", fname, 1, bits, typ))
            else:
                raw.append(("scalar", fname, 1, 0, typ))
            continue
        raise ValueError(
            f"struct '{sname}': unparseable field type '{typ} {fname}'")
    if not raw:
        raise ValueError(f"struct '{sname}': no fields parsed")
    # GBA layout: natural alignment (4-byte max), struct size rounded to 4.
    layout = []
    off = 0
    bitpos = 0
    prev_off = None
    prev_bit = False
    prev_bits = 0
    for kind, fname, count, bits, extra in raw:
        if kind in ("ptr", "func"):
            size, align = 4, 4
        elif kind == "ptrs":
            size, align = 4 * count, 4
        elif kind == "scalar":
            size, align = SCALAR_SIZE[extra], SCALAR_SIZE[extra]
        elif kind == "scalars":
            size, align = SCALAR_SIZE[extra] * count, SCALAR_SIZE[extra]
        elif kind == "bit":
            size, align = 1, 1
        off = (off + align - 1) & ~(align - 1)
        if kind == "bit":
            if prev_bit and off == prev_off:
                bitpos += prev_bits
            else:
                bitpos = 0
            layout.append((kind, fname, count, bits, off, size, extra,
                           bitpos))
            prev_off, prev_bit, prev_bits = off, True, bits
            continue  # consecutive bit fields pack into the same byte
        layout.append((kind, fname, count, bits, off, size, extra, 0))
        prev_off, prev_bit = off, False
        off += size
    gba_size = (off + 3) & ~3
    return raw, layout, gba_size


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def derive_key(canonical_name):
    """Gen3ResourceId_DeriveKey: SHA-256("gen3-resource-id-v1\\0" + name)."""
    return hashlib.sha256(
        b"gen3-resource-id-v1\0" + canonical_name.encode("ascii")).hexdigest()


class Elf32:
    """Minimal ELF32 symtab reader (mirrors the leaf generator)."""

    def __init__(self, data):
        if len(data) < 52 or data[0:4] != b"\x7fELF":
            fail("not an ELF file")
        if data[4] != 1 or data[5] != 1:
            fail("not ELFCLASS32 little-endian")
        self.data = data
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize = struct.unpack_from("<H", data, 0x2E)[0]
        shnum = struct.unpack_from("<H", data, 0x30)[0]
        self.sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            (sname, stype, sflags, saddr, soff, ssize, slink, sinfo,
             salign, sent) = struct.unpack_from("<IIIIIIIIII", data, off)
            self.sections.append(dict(name=sname, type=stype, flags=sflags,
                                      addr=saddr, offset=soff, size=ssize,
                                      link=slink, align=salign))
        shstr = self.sections[struct.unpack_from("<H", data, 0x32)[0]]
        self.shstr = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
        self.symtab = None
        for i, s in enumerate(self.sections):
            if s["type"] == 2:  # SHT_SYMTAB
                self.symtab = (i, s)
                break
        if self.symtab is None:
            fail("ELF has no SHT_SYMTAB")
        _, stab = self.symtab
        strtab = self.sections[stab["link"]]
        self.strtab = data[strtab["offset"]:strtab["offset"] + strtab["size"]]
        self.symbols = []  # (name, value, size, info, shndx)
        self.by_file = {}  # STT_FILE name (.o) -> [symbols]
        cur_file = None
        for off in range(stab["offset"], stab["offset"] + stab["size"], 16):
            (name_off, value, size, info, other, shndx) = struct.unpack_from(
                "<IIIBBH", data, off)
            end = self.strtab.find(b"\0", name_off)
            name = self.strtab[name_off:end].decode("ascii", "replace")
            sym = (name, value, size, info, shndx)
            self.symbols.append(sym)
            if info & 0xF == 4:  # STT_FILE: following locals belong to it
                cur_file = name
                continue
            if cur_file:
                self.by_file.setdefault(cur_file, []).append(sym)

    def sec_name(self, idx):
        s = self.sections[idx]
        end = self.shstr.find(b"\0", s["name"])
        return self.shstr[s["name"]:end].decode("ascii", "replace")

    def find(self, name, hint=None):
        """First symbol with `name`. With `hint` (a .c path whose .o the
        symbol must belong to), search that object's locals first — file-
        local statics collide across translation units (e.g. `sMessages`
        in berry_crush.c vs pokemon_storage_system.c)."""
        # Resolve through the recomp -> reference rename map so discovery
        # and slice see the same symbols resolve()/emit do (e.g. the older
        # revision's sMessages is the reference's sTradeMessages, present
        # only in trade.o's locals).
        name = RENAME_MAP.get(name, name)
        if hint:
            obj = hint.rsplit("/", 1)[-1][:-2] + ".o"
            for sym in self.by_file.get(obj, ()):
                if sym[0] == name:
                    return sym
        for sym in self.symbols:
            if sym[0] == name:
                return sym
        return None

    def slice(self, name, size, hint=None):
        sym = self.find(name, hint=hint)
        _, value, _, _, shndx = sym
        sect = self.sections[shndx]
        off = sect["offset"] + (value - sect["addr"])
        return self.data[off:off + size]


# --------------------------------------------------- charmap validator
# Gen3 escape grammar (include/constants/characters.h): 00-F6 single
# charmap-validated bytes, F7 dynamic, F8/F9/FC/FD two-byte escapes,
# FA/FB/FE singles, FF terminator must end the string.
def make_decode(charmap_path):
    single = set()
    double = {}
    for line in open(charmap_path, errors="ignore"):
        line = line.split("@")[0].strip()
        m = re.match(
            r"^(?:\S+\s*)?=\s*([0-9A-Fa-f]{1,2})(?:\s+([0-9A-Fa-f]{1,2}))?\s*$",
            line)
        if not m:
            continue
        b1 = int(m.group(1), 16)
        if m.group(2):
            double[(b1, int(m.group(2), 16))] = line
        else:
            single.add(b1)

    def decode(b):
        i = 0
        while i < len(b):
            c = b[i]
            if c == 0xFF:
                return i == len(b) - 1
            if c in (0xF8, 0xF9, 0xFC, 0xFD):
                if i + 1 >= len(b):
                    return False
                i += 2
                continue
            if c in single:
                i += 1
                continue
            return False
        return False  # no terminator

    return decode


# ---------------------------------------------------------- source scans
LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):{1,2}\s*(?:@.*)?$")
STRING_DIRECTIVE_RE = re.compile(r"\s*\.(string|ascii)\b")


def scan_asm_text(path):
    """Labels whose body begins with .string/.ascii (lookahead past
    comments/blanks). Returns {label: None} (path known by caller)."""
    labels = {}
    try:
        lines = open(path, errors="ignore").read().splitlines()
    except OSError:
        return labels
    for i, ln in enumerate(lines):
        m = LABEL_RE.match(ln)
        if not m:
            continue
        j = i + 1
        while j < len(lines) and (not lines[j].strip()
                                  or lines[j].lstrip().startswith("@")):
            j += 1
        if j < len(lines) and STRING_DIRECTIVE_RE.match(lines[j]):
            labels[m.group(1)] = None
    return labels


ARRAY_RE = re.compile(
    r"\s*(?:ALIGNED\(\s*\d+\s*\)\s*)?(?:static\s+)?const\s+u8\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]")


def c_labels(text):
    """const u8 NAME[] definitions with a _( ... ) or = "..." initializer
    within the declaration + 6 lines (the pin.py rule)."""
    out = set()
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        m = ARRAY_RE.match(ln)
        if not m:
            continue
        for j in range(i, min(i + 7, len(lines))):
            if re.search(r"_\s*\(", lines[j]) or re.search(r'=\s*"', lines[j]):
                out.add(m.group(1))
                break
    return out


ASM_FAMILY = {
    "data/text/trainers.inc": "trainer",
    "data/text/match_call.inc": "match-call",
    "data/text/tv.inc": "tv",
    "data/text/apprentice.inc": "apprentice",
    "data/text/battle_dome.inc": "frontier",
    "data/text/battle_tent.inc": "frontier",
    "data/text/cable_club.inc": "cable-club",
    "data/text/berries.inc": "berry",
    "data/text/frontier_brain.inc": "frontier-brain",
    "data/text/mauville_man.inc": "mauville-man",
    "data/text/mart_clerk.inc": "mart",
    "data/text/pokedex_rating.inc": "pokedex-rating",
    "data/text/pokemon_news.inc": "pokemon-news",
}
CONTEST_LINK_MISC = frozenset(
    {"gTest_MissedTurn", "gText_LinkTVProgramWillNotBeMadeTrainerLost"})


def asm_family(path, label):
    p = path.replace("\\", "/")
    if p.startswith("data/maps/"):
        return "map-dialogue"
    if p.startswith("data/scripts/"):
        return "misc-scripts"
    if p == "data/event_scripts.s":
        return "misc"
    if p == "data/text/contest_link.inc":
        # Plan misc = event_scripts.s body (25) + contest_link.inc (2);
        # the rest of contest_link.inc folds into system (AC-1 §2b).
        return "misc" if label in CONTEST_LINK_MISC else "system"
    if p in ASM_FAMILY:
        return ASM_FAMILY[p]
    return "system"  # asm-misc rest -> plan system (AC-1 §2a)


C_TEXT_HEADER = {
    "abilities.h": "ability", "gift_ribbon_descriptions.h": "ribbon",
    "item_descriptions.h": "item", "match_call_messages.h": "match-call",
    "move_descriptions.h": "move", "nature_names.h": "nature",
    "ribbon_descriptions.h": "ribbon",
}


def c_family(path, label):
    p = path.replace("\\", "/")
    if p == "src/strings.c":
        return "shared" if label.startswith("gText_") else "system"
    if p == "src/battle_message.c":
        return "battle"
    if p.startswith("src/data/text/"):
        return C_TEXT_HEADER.get(p[len("src/data/text/"):], "c-misc")
    if p.startswith("src/data/pokemon/"):
        return "pokedex"
    if p.startswith("src/data/easy_chat/"):
        return "easy-chat"
    return "c-misc"


def canonical(symbol):
    """Canonical key component: lowercase symbol with '_' -> '-' (the
    R11-B rule). One documented collision group exists (AC-3:
    geasychatword-were); assign_label_keys() disambiguates it."""
    return symbol.lower().replace("_", "-")


def assign_label_keys(c_records):
    """{label: resource id} for every C-side label. Keys are
    emerald:text/<segment>/<canonical>; the single documented collision
    group (geasychatword-were: WeRe/Were) is disambiguated by sorted
    symbol order: the first symbol gets the base key, the rest get
    deterministic -1/-2/... ordinals (documented key-canonicalization
    note; the base rule itself is unchanged)."""
    from collections import defaultdict
    fam = {r[0]: r[1] for r in c_records}
    groups = defaultdict(list)
    for r in c_records:
        groups[canonical(r[0])].append(r[0])
    keymap = {}
    for can, syms in sorted(groups.items()):
        for i, sym in enumerate(sorted(syms)):
            suffix = "" if i == 0 else f"-{i}"
            keymap[sym] = f"emerald:text/{KEY_SEGMENT[fam[sym]]}/{can}{suffix}"
    return keymap


# ------------------------------------------------------------- inventory
def enumerate_sources(root):
    """{label: (path, kind)} - the AC-1 scope. kind in {'asm', 'c'}."""
    src = {}
    for dirpath, dirnames, filenames in os_walk(root / "data"):
        for f in filenames:
            p = os_path(dirpath, f)
            rel = os.path.relpath(p, start=str(root)).replace("\\", "/")
            d = rel.rsplit("/", 1)[0] if "/" in rel else ""
            if f.endswith(".inc") and (
                    d.endswith(("/text", "/scripts")) or "/maps/" in rel):
                for lab in scan_asm_text(p):
                    src.setdefault(lab, (rel, "asm"))
            elif f.endswith(".s") and rel.startswith("data/") \
                    and "/" not in rel[len("data/"):]:
                for lab in scan_asm_text(p):
                    src.setdefault(lab, (rel, "asm"))
    c_files = ["src/strings.c", "src/battle_message.c"]
    c_files += sorted(str(Path(r) / f) for r, d, fs in os_walk(root / "src/data/text")
                      for f in fs if f.endswith(".h"))
    c_files += sorted(str(Path(r) / f) for r, d, fs in os_walk(root / "src/data/pokemon")
                      for f in fs if f.endswith(".h"))
    c_files += sorted(str(Path(r) / f) for r, d, fs in os_walk(root / "src/data/easy_chat")
                      for f in fs if f.endswith(".h"))
    for rel in c_files:
        try:
            text = open(root / rel, errors="ignore").read()
        except OSError:
            continue
        for lab in c_labels(text):
            src.setdefault(lab, (rel, "c"))
    return src


def os_walk(d):
    return os.walk(d)


def os_path(d, f):
    return os.path.join(d, f)


# ------------------------------------------------------------ resolution
def resolve(root, elf, rom, decode, args):
    """Resolve every source label against the qualified ELF + ROM.
    Returns (records, asm_files) where records = list of
    (label, family, path, kind, section, rom_off, size) and asm_files =
    {path: [labels]} for every asm file with >= 1 resolved label."""
    src = enumerate_sources(root)
    ooc = {name for name, _, _ in OUT_OF_CONTRACT}
    if len(ooc) != 13:
        fail(f"out-of-contract list has {len(ooc)} entries != 13")

    resolved = {}   # label -> (path, kind, section, rom_off, size)
    unresolved = []
    for label, (path, kind) in sorted(src.items()):
        elf_name = RENAME_MAP.get(label, label)
        sym = elf.find(elf_name)
        if sym is None:
            unresolved.append((label, path))
            continue
        value = sym[1]
        off = value - GEN3_GBA_ROM_BASE
        if off < 0 or off >= len(rom):
            fail(f"label '{label}' resolves outside the ROM image")
        n = 0
        while off + n < len(rom) and n < PINNED_SCAN_BOUND \
                and rom[off + n] != 0xFF:
            n += 1
        if off + n >= len(rom) or n >= PINNED_SCAN_BOUND:
            fail(f"label '{label}': no 0xFF terminator within the "
                 f"{PINNED_SCAN_BOUND}-byte bound")
        size = n + 1
        # gText_123Dot is a 3-row byte array ("1.\xff2.\xff3.\xff",
        # 9 B): the run-to-0xFF scan covers only the first row. The ELF
        # symbol span is the authority (fail-closed: exactly 9 B) and
        # each 3-byte row must pass the charmap grammar on its own -
        # the concatenated payload never does (mid-payload 0xFF). The
        # 9-byte record keeps the seam's base + fill->row offsets
        # (0/3/6) inside one payload (R13-C #112).
        if label == "gText_123Dot":
            if sym[2] != 9:
                fail(f"label 'gText_123Dot': ELF span {sym[2]} != 9 "
                     f"(3 rows x 3 B)")
            size = 9
            for row_off in range(0, size, 3):
                if not decode(rom[off + row_off:off + row_off + 3]):
                    fail(f"label 'gText_123Dot': row {row_off // 3} "
                         f"charmap grammar validation failed")
        elif not decode(rom[off:off + size]):
            fail(f"label '{label}': charmap grammar validation failed "
                 f"({size} B from 0x{value:08x})")
        section = elf.sec_name(sym[4])
        if section not in ("script_data", ".rodata"):
            fail(f"label '{label}': unexpected section '{section}'")
        resolved[label] = (path, kind, section, off, size)

    # The out-of-contract labels must be exactly the unresolved set.
    unresolved_names = {name for name, _ in unresolved}
    if unresolved_names != ooc:
        missing = sorted(ooc - unresolved_names)
        extra = sorted(unresolved_names - ooc)
        fail(f"out-of-contract mismatch: present-but-should-be-absent "
             f"{missing}; unresolved-extra {extra}")

    records = []   # (label, family, path, kind, section, off, size)
    for label, (path, kind, section, off, size) in sorted(resolved.items()):
        if kind == "asm":
            fam = asm_family(path, label)
        else:
            fam = c_family(path, label)
        if fam == "c-misc":
            continue  # gSpeciesNames/gTrainerClassNames: not text strings
        records.append((label, fam, path, kind, section, off, size))
    return records


# --------------------------------------------------------------- gates
def check_pins(records):
    total = len(records)
    total_b = sum(r[6] for r in records)
    if total != PINNED_TOTAL_LABELS or total_b != PINNED_TOTAL_BYTES:
        fail(f"total {total} labels / {total_b} B != pinned "
             f"{PINNED_TOTAL_LABELS} / {PINNED_TOTAL_BYTES}")

    script = [r for r in records if r[4] == "script_data"]
    rodata = [r for r in records if r[4] == ".rodata"]
    if len(script) != PINNED_SCRIPT_LABELS \
            or sum(r[6] for r in script) != PINNED_SCRIPT_BYTES:
        fail(f"script_data {len(script)} labels / "
             f"{sum(r[6] for r in script)} B != pinned "
             f"{PINNED_SCRIPT_LABELS} / {PINNED_SCRIPT_BYTES}")

    c_side = [r for r in records if r[3] == "c"]
    asm_side = [r for r in records if r[3] == "asm"]
    if len(asm_side) != PINNED_ASM_LABELS \
            or sum(r[6] for r in asm_side) != PINNED_ASM_BYTES:
        fail(f"asm-source {len(asm_side)} / {sum(r[6] for r in asm_side)} B "
             f"!= pinned {PINNED_ASM_LABELS} / {PINNED_ASM_BYTES}")
    if len(c_side) != PINNED_C_RESOURCES \
            or sum(r[6] for r in c_side) != PINNED_C_RESOURCE_BYTES:
        fail(f"C-side resources {len(c_side)} / {sum(r[6] for r in c_side)} "
             f"B != pinned {PINNED_C_RESOURCES} / {PINNED_C_RESOURCE_BYTES}")

    # Gift labels: asm-source labels resolving in .rodata.
    gift = [r for r in records if r[3] == "asm" and r[4] == ".rodata"]
    if len(gift) != PINNED_GIFT_LABELS or sum(r[6] for r in gift) != PINNED_GIFT_BYTES:
        fail(f"gift labels {len(gift)} / {sum(r[6] for r in gift)} B != "
             f"pinned {PINNED_GIFT_LABELS} / {PINNED_GIFT_BYTES}")

    # Per-family pins (AC-1 §2 authoritative table).
    from collections import Counter
    fam_c = Counter(r[1] for r in records)
    fam_b = Counter()
    for r in records:
        fam_b[r[1]] += r[6]
    if set(fam_c) != set(FAMILY_PINS):
        fail(f"family set mismatch: {sorted(fam_c)} vs "
             f"{sorted(FAMILY_PINS)}")
    for fam, (lc, bc) in sorted(FAMILY_PINS.items()):
        if fam_c[fam] != lc or fam_b[fam] != bc:
            fail(f"family {fam}: {fam_c[fam]} / {fam_b[fam]} B != pinned "
                 f"{lc} / {bc} B")

    # Size distribution pins.
    sizes = sorted(r[6] for r in records)
    if sizes[0] != 1 or sizes[len(sizes) // 2] != 49 or sizes[-1] != 824:
        fail(f"min/median/max sizes {sizes[0]}/{sizes[len(sizes)//2]}/"
             f"{sizes[-1]} != 1/49/824")
    empty = sum(1 for r in records if r[6] == 1)
    single = sum(1 for r in records if r[6] == 2)
    if empty != PINNED_EMPTY or single != PINNED_SINGLE:
        fail(f"empty {empty} / single-char {single} != pinned "
             f"{PINNED_EMPTY} / {PINNED_SINGLE}")

    # Alias + duplicate-byte groups (kept - plan §4, no dedup).
    addr = {}
    for r in records:
        addr.setdefault(r[5], []).append(r[0])
    alias = {a: n for a, n in addr.items() if len(n) > 1}
    if len(alias) != PINNED_ALIAS_GROUPS:
        fail(f"same-address alias groups {len(alias)} != "
             f"{PINNED_ALIAS_GROUPS}")
    bymap = {}
    for r in records:
        bymap.setdefault(bytes(rom[r[5]:r[5] + r[6]]), []).append(r[0])
    dups = {b: n for b, n in bymap.items() if len(n) > 1}
    dup_syms = sum(len(n) for n in dups.values())
    redundant = sum(len(b) * (len(n) - 1) for b, n in dups.items())
    if len(dups) != PINNED_DUP_GROUPS or dup_syms != PINNED_DUP_SYMBOLS \
            or redundant != PINNED_REDUNDANT_BYTES:
        fail(f"dup groups {len(dups)} / symbols {dup_syms} / redundant "
             f"{redundant} B != pinned {PINNED_DUP_GROUPS} / "
             f"{PINNED_DUP_SYMBOLS} / {PINNED_REDUNDANT_BYTES}")

    # Canonical-name collision check under the dash-lowercase rule. Exactly
    # one collision exists (AC-3, measured): geasychatword-were =
    # {gEasyChatWord_WeRe, gEasyChatWord_Were} - the pair differs only in
    # case, so ANY case-folding canonical rule collides. Resolution is a
    # deterministic ordinal (sorted-symbol order: base key to the first
    # symbol, -1/-2/... to the rest), assigned in assign_label_keys().
    from collections import defaultdict
    groups = defaultdict(list)
    for r in records:
        if r[3] == "c":
            groups[canonical(r[0])].append(r[0])
    coll = {k: sorted(v) for k, v in groups.items() if len(v) > 1}
    if len(coll) != len(PINNED_KEY_COLLISION_GROUPS) or \
            {k: tuple(v) for k, v in coll.items()} != \
            {k: tuple(v) for k, v in PINNED_KEY_COLLISION_GROUPS.items()}:
        fail(f"canonical key collision groups {coll} != pinned "
             f"{PINNED_KEY_COLLISION_GROUPS}")

    # No C-side label referenced from any asm source (script operands must
    # not alias a slot-converted symbol - fail-closed before any
    # ROM_BASE_ONLY assignment).
    asm_corpus = []
    import os
    for dirpath, dirnames, filenames in os.walk(str(root / "data")):
        for f in filenames:
            if f.endswith((".inc", ".s")):
                asm_corpus.append(
                    open(os.path.join(dirpath, f), errors="ignore").read())
    asm_text = "\n".join(asm_corpus)
    c_names = sorted(r[0] for r in records if r[3] == "c")
    pattern = re.compile(r"\b(?:" + "|".join(re.escape(n) for n in c_names)
                         + r")\b")
    hits = sorted(set(pattern.findall(asm_text)))
    if hits:
        fail(f"{len(hits)} C-side labels referenced from asm sources "
             f"(script operand aliasing): {hits[:10]}")
    return records


# ------------------------------------------------------------- dual-ref
def strip_comments(text):
    """Remove //-to-EOL and /* ... */ comments WITHOUT deleting the
    newlines they span, so line shapes (and hence line-classified scans)
    are unchanged. (A stripper that dropped the newline after // comments
    merged two rows into one line and corrupted the dual-ref sweep —
    AC-4.)"""
    t = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"/\*.*?\*/",
                  lambda m: "\n" * m.group(0).count("\n"),
                  t, flags=re.S)


def classify_line(s):
    """AC-1 §5 line-kind classifier (line-classified, per the proven
    method): bare `L,` / `[expr] = L,` / `{ ... },` rows are array
    entries (NOT references); extern/typedef lines are declarations;
    anything else is a direct code reference."""
    if s.startswith(("extern ", "typedef ")):
        return "decl"
    s = re.sub(r",?\s*$", "", s)
    if s.endswith("}"):
        s = s[:-1].rstrip()
    if re.fullmatch(r"\[[^\[\]]*\]\s*=\s*[A-Za-z_][A-Za-z0-9_]*", s):
        return "row"
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", s):
        return "row"
    if s.startswith("{"):
        return "row"
    return "code"


def find_dual_refs(root, asm_labels):
    """Asm labels with a direct C code reference (line-classified per the
    AC-1 §5 documented method: each line is classified once as decl / row
    / code; table rows and declarations are not references). Returns
    {label: first ref site}."""
    asm_set = set(asm_labels)
    dual = {}
    for rel in sorted(str(Path(r) / f)
                      for r, d, fs in os_walk(root / "src")
                      for f in fs if f.endswith(".c")):
        text = strip_comments(open(root / rel, errors="ignore").read())
        for ln, raw in enumerate(text.splitlines(), 1):
            s = raw.strip()
            if not s or classify_line(s) != "code":
                continue
            for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\b", s):
                label = m.group(1)
                if label in asm_set and label not in dual:
                    dual[label] = f"{rel}:{ln}"
    return dual


# ------------------------------------- text-table discovery (R13-C §9/§10)
class TextTable:
    """One discovered native text-pointer table to cut over."""
    __slots__ = ("name", "rel", "sname", "star", "shape", "bytes", "words",
                 "body", "kind", "fills", "init_rows", "externs",
                 "gba_size", "source_only", "rows", "inner", "macros",
                 "_dropped", "host_name", "scalar", "compiled_externs",
                 "mixed", "rewrite_discards")

    def __init__(self, name, rel, sname, star, shape, table_bytes, body):
        self.name = name
        self.host_name = name       # unique C symbol; set at analyze end
        self.rel = rel
        self.sname = sname          # struct row type ("" for pure tables)
        self.star = star            # `struct T *const` rows
        self.shape = shape          # bracket contents, outer first
        self.bytes = table_bytes    # ELF row bytes (empty for source-only)
        self.words = list(struct.unpack(
            "<%dI" % (len(table_bytes) // 4), table_bytes))
        self.body = body            # comment-stripped source initializer
        self.kind = "unknown"       # text / subtable
        self.fills = []             # (target_expr, fill_kind, name)
        self.init_rows = None       # struct tables: initializer lines
        self.externs = []           # struct tables: (ret, name, params)
        self.gba_size = 0           # struct tables: stride in bytes
        self.source_only = False    # recomp-only table (no ELF slice)
        self.rows = 0               # row count (ELF- or source-derived)
        self.macros = []            # (row, source) macro-initializer rows
        self.inner = 1              # pure/star tables: inner width
        self._dropped = False       # analyze deferral: not emitted
        self.scalar = False         # scalar struct object (no brackets)
        self.compiled_externs = {}  # compiled-constant rows/fields:
                                    # sym -> (full type, shape) re-emitted
                                    # as externs in the host arrays .c
        self.mixed = False          # recomp-only mixed-family table
        self.rewrite_discards = set()  # ROM labels replaced by recomp
                                       # rewrites (coverage-check exempt)


TABLE_DECL_RE = re.compile(
    r"(?m)^\s*(?:(?:static|const|ALIGNED\(\s*\d+\s*\))\s+)*"
    r"(?:u8\s*\*(?:\s*const)?\s+|struct\s+(\w+)\s*(\*\s*const)?\s+)"
    r"([A-Za-z_]\w*)\s*((?:\[[^\]]*\])+)\s*=\s*\{")


def table_body_from(text, start):
    """Balanced-brace body of the initializer whose '{' sits at `start`
    (comment-stripped text; comments can't spoof the scan)."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        i += 1
    return None


def split_top_level(body):
    """Top-level comma segments (row tokens), tolerant of multi-label
    lines, designated rows and nested braces."""
    segs = []
    depth = 0
    cur = []
    for ch in body:
        if ch in "{[(":
            depth += 1
        elif ch in "}])":
            depth -= 1
        cur.append(ch)
        if ch == "," and depth == 0:
            # The separator comma is the split point, not row content:
            # exclude it from the segment (a trailing comma leaked into
            # compiled-constant field expressions via _field_values'
            # rest[1:-1], e.g. sDexSearchTypeOptions rows became
            # "gTypeNames[0]}").
            s = "".join(cur).strip().rstrip(",")
            if s:
                segs.append(s)
            cur = []
    s = "".join(cur).strip().rstrip(",")
    if s:
        segs.append(s)
    return segs


LABEL_TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
MACRO_CALL_RE = re.compile(r"(\w+)\s*\(([^()]*)\)")
MACRO_DEF_RE = re.compile(r"^\s*#\s*define\s+(\w+)\s*(?:\(([^)]*)\))?"
                          r"\s*(.*?)\s*$", re.M)


def _join_continuations(text):
    return re.sub(r"\\\s*\n", "", text)


def _macro_defs(texts):
    """{name: (params, body)} from comment-stripped, continuation-joined
    macro definitions."""
    out = {}
    for tx in texts:
        for m in MACRO_DEF_RE.finditer(_join_continuations(tx)):
            if m.group(2) is None:
                params = []
            else:
                params = [p.strip() for p in m.group(2).split(",")]
            out[m.group(1)] = (params, m.group(3))
    return out


def expand_macro_calls(text, macro_defs, depth=0):
    """Replace `NAME(args)` initializer macros (e.g. MCFLAVOR(Brendan))
    with their substituted bodies so source rows can be token-verified
    against the ELF. `##` token pasting is handled textually (pieces are
    identifiers). Bounded recursion; unresolvable calls raise ValueError
    (fail-closed)."""
    if depth > 3:
        raise ValueError("macro expansion depth exceeded")
    def sub(m):
        name, args = m.group(1), m.group(2)
        if name not in macro_defs:
            raise ValueError(f"macro '{name}' not defined in the table's "
                             f"translation unit")
        params, body = macro_defs[name]
        a = [x.strip() for x in args.split(",")] if args.strip() else []
        if len(params) != len(a):
            raise ValueError(f"macro '{name}' expects {len(params)} args, "
                             f"got {len(a)}")
        out = body
        for p, av in zip(params, a):
            out = re.sub(r"##\s*" + re.escape(p) + r"\s*##",
                         av, out)
            out = re.sub(r"##\s*" + re.escape(p) + r"\b", av, out)
            out = re.sub(r"\b" + re.escape(p) + r"\s*##", av, out)
            out = re.sub(r"\b" + re.escape(p) + r"\b", av, out)
        out = out.replace("##", "")
        return expand_macro_calls(out, macro_defs, depth + 1)
    return MACRO_CALL_RE.sub(sub, text)


def segment_label_tokens(seg, known_labels):
    """Known-inventory label tokens in one source row."""
    return [t for t in LABEL_TOKEN_RE.findall(seg) if t in known_labels]


def _tu_texts(root, rel):
    """TU text plus its direct includes, resolved the way the compiler
    does: the TU's own directory first, then include/. Used to gather
    enum bounds and object/function macros (e.g. MCFLAVOR) for source
    row verification.

    Header fragments (src/data/*/*.h) carry no includes of their own;
    the macros in effect at their rows come from the TU(s) that include
    the fragment (e.g. MCFLAVOR in pokenav.h for
    match_call_messages.h rows via pokenav_match_call_list.c), so those
    TU texts and their direct includes are appended too."""
    texts = []
    seen = {root / rel}

    def append_with_includes(path):
        p = root / path
        if not p.exists():
            return
        texts.append(open(p, errors="ignore").read())
        for im in re.finditer(r'#\s*include\s*"([^"]+)"', texts[-1]):
            for cand in (root / Path(path).parent / im.group(1),
                         root / "include" / im.group(1)):
                if cand.exists() and cand not in seen:
                    seen.add(cand)
                    texts.append(open(cand, errors="ignore").read())

    append_with_includes(rel)
    if rel.endswith(".h"):
        frag = rel[4:] if rel.startswith("src/") else rel
        for c in sorted((root / "src").glob("*.c")):
            body = open(c, errors="ignore").read()
            if re.search(r'#\s*include\s*"%s"' % re.escape(frag), body):
                append_with_includes(c)
    return texts


def _close_brace(text, start):
    """Index of the '}' closing the '{' at `start` (comment-stripped)."""
    depth = 0
    for i in range(start, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
    return None


# Scalar struct objects: `static const struct Tag NAME = { ... }` (no
# brackets) whose pointer fields can reference slots (NamingScreenTemplate
# objects, sMrStoneMatchCallHeader).
SCALAR_DECL_RE = re.compile(
    r"(?m)^\s*(?:(?:static|const|ALIGNED\(\s*\d+\s*\))\s+)*"
    r"struct\s+(\w+)\s+([A-Za-z_]\w*)\s*=\s*\{")

# Typedef'd struct arrays: `static const match_call_text_data_t NAME[] = {`
# (TABLE_DECL_RE requires a `struct Tag`). Scalar-typed arrays (u8 sX[])
# match too but can never be text tables — pass 1 filters them.
TYPEDEF_ARRAY_RE = re.compile(
    r"(?m)^\s*(?:(?:static|const|ALIGNED\(\s*\d+\s*\))\s+)*"
    r"([A-Za-z_]\w+)\s+([A-Za-z_]\w*)\s*((?:\[[^\]]*\])+)\s*=\s*\{")
_SCALAR_TYPES = {"u8", "s8", "u16", "s16", "u32", "s32", "u64", "s64",
                 "bool8", "bool16", "bool32", "void", "char", "int",
                 "short", "long", "float", "double", "struct", "const",
                 "signed", "unsigned", "size_t"}

# Struct instances: `struct Tag { ... } static const NAME[] = { ... }`
# (the tag precedes the modifiers — sMainMenuTexts, sCursorOptions,
# sMapLandmarks are all anonymous-but-tagged this way).
ANON_STRUCT_INSTANCE_RE = re.compile(
    r"(?m)^\s*struct\s+(\w+)\s*\{")


def _decl_full(texts, sym):
    """(type, shape) of `sym`'s declaration in `texts`, or None:
    ('const u8 *const', '[3][20]') for
    `static const u8 *const sNamingScreenKeyboardText[3][20]`. The host
    arrays .c re-emits the extern with the full pointer type and shape
    so compiled-constant rows/fields stay type-correct. Pointer objects
    (no brackets) return an empty shape."""
    for tx in texts:
        m = re.search(r"(?m)(?:extern\s+)?(?:static\s+)?"
                      r"((?:const\s+)?(?:struct\s+)?[A-Za-z_]\w*"
                      r"(?:\s*[*]+(?:\s*const\s*)?)*)\s+"
                      + re.escape(sym)
                      + r"((?:\s*\[[^\]]*\])+|)(?=\s*[=;])", tx)
        if m:
            return m.group(1).strip(), m.group(2).strip()
    return None


def _const_value(ident, texts, pool):
    """int value of a constant identifier: numeric #define or enum member
    in `pool` (include/constants/*.h) or `texts` (TU + includes)."""
    if re.match(r"^\d+$", ident):
        return int(ident)
    for src in pool + texts:
        m = re.search(r"(?m)^\s*#\s*define\s+" + re.escape(ident)
                      + r"\s+(-?\d+)\s*$", src)
        if m:
            return int(m.group(1))
    for src in pool + texts:
        v = resolve_enum_const(src, ident)
        if v is not None:
            return v
    raise ValueError(f"constant '{ident}' not resolvable")


_EXPR_TOKEN_RE = re.compile(r"\d+|[A-Za-z_]\w*|[()+*/%-]")


def _eval_int_expr(expr, texts, pool):
    """Evaluate a constant C integer expression (identifiers, + - * / %
    and parentheses). Fail-closed: any unresolved identifier or stray
    token raises ValueError."""
    toks = _EXPR_TOKEN_RE.findall(expr)
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def parse_expr():
        v = parse_term()
        while peek() in ("+", "-"):
            op = peek()
            pos[0] += 1
            r = parse_term()
            v = v + r if op == "+" else v - r
        return v

    def parse_term():
        v = parse_factor()
        while peek() in ("*", "/", "%"):
            op = peek()
            pos[0] += 1
            r = parse_factor()
            if op == "*":
                v = v * r
            elif op == "/":
                v = v // r
            else:
                v = v % r
        return v

    def parse_factor():
        t = peek()
        if t is None:
            raise ValueError("empty expression")
        if t == "(":
            pos[0] += 1
            v = parse_expr()
            if peek() != ")":
                raise ValueError("unbalanced '(' in expression")
            pos[0] += 1
            return v
        if t == "-":
            pos[0] += 1
            return -parse_factor()
        if t.isdigit():
            pos[0] += 1
            return int(t)
        if t and t[0].isalpha():
            pos[0] += 1
            return _const_value(t, texts, pool)
        raise ValueError(f"unexpected token '{t}'")

    if not toks:
        raise ValueError("empty expression")
    v = parse_expr()
    if pos[0] != len(toks):
        raise ValueError(f"trailing tokens in '{expr}'")
    return v


_BRACKET_EXPR_RE = re.compile(r"\[([^\]]+)\]")


def _eval_expr_brackets(expr, texts, pool):
    """Replace [CONST] array indices with their numeric values so the
    generated arrays .c needs no data-header includes (e.g.
    gTypeNames[TYPE_NORMAL] -> gTypeNames[8])."""
    def sub(m):
        return f"[{_eval_int_expr(m.group(1), texts, pool)}]"
    return _BRACKET_EXPR_RE.sub(sub, expr)


def _row_designator(seg, texts, pool):
    """(index, rest) for a designated row `[EXPR] = VALUE` (index
    evaluated numerically, fail-closed); (None, seg) when undesignated."""
    m = re.match(r"\[(.+)\]\s*=\s*(.*)$", seg, re.S)
    if m is None:
        return None, seg.strip()
    try:
        idx = _eval_int_expr(m.group(1), texts, pool)
    except ValueError as e:
        raise ValueError(f"designator '{m.group(1)}': {e}")
    return idx, m.group(2).strip()


def _field_values(rest, layout):
    """{fname: initializer expr} for one struct row's value text (the
    part after any designator), supporting `.fname = EXPR` and positional
    values in layout order (fail-closed on count mismatch). Rows carry
    braces (`{a, b}`); scalar objects arrive brace-less (the outer
    braces were consumed by the body scan) and split on top-level
    commas too; a bare scalar value stays one segment."""
    if rest.startswith("{"):
        vals = split_top_level(rest[1:-1])
    elif rest.startswith(".") or "\n" in rest:
        vals = split_top_level(rest)
    else:
        vals = [rest]
    fvals = {}
    pend = []
    for v in vals:
        m = re.match(r"\.(\w+)\s*=\s*(.*)$", v, re.S)
        if m:
            fvals[m.group(1)] = m.group(2).strip()
        else:
            pend.append(v.strip())
    ppos = 0
    for (kind, fname, count, bits, off, size, extra, bitpos) in layout:
        if fname in fvals:
            continue
        if ppos < len(pend):
            fvals[fname] = pend[ppos]
            ppos += 1
    if ppos != len(pend):
        raise ValueError(f"{len(pend)} positional values for "
                         f"{len([f for f in layout if f[1] not in fvals])} "
                         f"remaining fields")
    return fvals


def _field_value(seg, fname, layout, texts, pool, sym):
    """Source initializer expression for one struct field whose word is
    the compiled symbol `sym` (fail-closed when the row does not name
    it), with array indices evaluated."""
    _, rest = _row_designator(seg, texts, pool)
    fvals = _field_values(rest, layout)
    if fname not in fvals:
        raise ValueError(f"field '{fname}' has no source value in row "
                         f"'{seg}'")
    src = fvals[fname]
    if sym not in LABEL_TOKEN_RE.findall(src):
        raise ValueError(f"field '{fname}' source expression '{src}' "
                         f"does not reference '{sym}'")
    return _eval_expr_brackets(src, texts, pool).lstrip("&")


def fmt_scalar(typ, val, size):
    """C initializer literal for a little-endian scalar/array element."""
    if typ in ("u8", "bool8"):
        return "0x%02x" % val
    if typ == "s8":
        return str(val if val < 0x80 else val - 0x100)
    if typ == "u16":
        return "0x%04x" % val
    if typ == "s16":
        return str(val if val < 0x8000 else val - 0x10000)
    if typ == "u32":
        return "0x%08xu" % val
    if typ == "s32":
        return str(val if val < 0x80000000 else val - 0x100000000)
    raise ValueError(f"scalar type '{typ}' outside the R13-C model")


def discover_text_tables(root, elf, records):
    """Discover every live text-pointer table per the R13-C rules.
    Returns the ordered [TextTable] (fail-closed: family mix, ELF
    absence with known-label rows, non-multiple-of-4 sizes all fail)."""
    known = {}
    for r in records:
        known[r[0]] = r
    addr_of = {r[5] + GEN3_GBA_ROM_BASE: r[0] for r in records}
    by_addr = {}
    for sym in elf.symbols:
        if sym[0] and not sym[0].startswith(".") and sym[3] & 0xF != 0 \
                and sym[1] != 0 and sym[2] != 0:
            by_addr.setdefault(sym[1], sym[0])
    files = sorted(str(p) for p in (root / "src").glob("*.c"))
    files += sorted(str(p) for p in (root / "src/data/text").glob("*.h"))
    files += [str(root / "src/data/contest_text_tables.h"),
              str(root / "src/data/script_menu.h"),
              str(root / "src/data/party_menu.h"),
              str(root / "src/data/trade.h"),
              str(root / "src/data/lilycove_lady.h"),
              str(root / "src/data/union_room.h"),
              str(root / "src/data/battle_frontier/"
                      "battle_frontier_exchange_corner.h")]
    tables = []
    deferred = []  # (rel, name, families): mixed live/deferred, stays
                   # compiled per the map/script-stage deferral boundary
    raw = []
    # (rel, name, sname, star, shape, body, scalar) for every candidate
    # declaration in the scan list (R13-C discovery gaps: typedef'd
    # struct arrays, scalar struct objects, struct instances).
    decls = []
    for rel in files:
        text = strip_comments(open(root / rel, errors="ignore").read())
        for m in TABLE_DECL_RE.finditer(text):
            sname = m.group(1) or ""
            name = m.group(3)
            if name in ("gSpeciesNames", "gTrainerClassNames"):
                continue
            star = bool(m.group(2))
            shape = [s for s in re.findall(r"\[([^\]]*)\]", m.group(4))]
            j = text.find("{", m.end() - 1)
            body = table_body_from(text, j)
            if body is None:
                fail(f"table '{name}': braces unbalanced in {rel}")
            decls.append((rel, name, sname, star, shape, body, False))
        for m in SCALAR_DECL_RE.finditer(text):
            sname, name = m.group(1), m.group(2)
            j = text.find("{", m.end() - 1)
            body = table_body_from(text, j)
            if body is None:
                fail(f"table '{name}': braces unbalanced in {rel}")
            decls.append((rel, name, sname, False, [], body, True))
        for m in ANON_STRUCT_INSTANCE_RE.finditer(text):
            j = text.find("{", m.end() - 1)
            end = _close_brace(text, j)
            if end is None:
                continue
            tm = re.match(r"(?s)\s*(?:(?:static|const)\s+)*"
                          r"([A-Za-z_]\w*)\s*((?:\[[^\]]*\])+)\s*=\s*\{",
                          text[end + 1:end + 1 + 160])
            if tm is None:
                continue
            name = tm.group(1)
            if name in ("gSpeciesNames", "gTrainerClassNames"):
                continue
            shape = [s for s in re.findall(r"\[([^\]]*)\]", tm.group(2))]
            j2 = text.find("{", end + 1 + tm.start())
            body = table_body_from(text, j2)
            if body is None:
                fail(f"table '{name}': braces unbalanced in {rel}")
            decls.append((rel, name, m.group(1), False, shape, body, False))
        for m in TYPEDEF_ARRAY_RE.finditer(text):
            typ, name = m.group(1), m.group(2)
            if typ in _SCALAR_TYPES:
                continue
            if name in ("gSpeciesNames", "gTrainerClassNames"):
                continue
            # Resolve the typedef to its struct tag (e.g.
            # match_call_text_data_t -> MatchCallTextDataStruct); the
            # struct parse and the host arrays type the tag.
            tag = None
            for tx in [text] + [strip_comments(open(root / r, errors="ignore").read())
                                for r in (root / "include").glob("*.h")]:
                mm = re.search(r"typedef\s+struct\s+(\w+)\s*\{.*?\}\s*"
                               + re.escape(typ) + r"\s*;", tx, re.S)
                if mm:
                    tag = mm.group(1)
                    break
            if tag is None:
                continue
            shape = [s for s in re.findall(r"\[([^\]]*)\]", m.group(3))]
            j = text.find("{", m.end() - 1)
            body = table_body_from(text, j)
            if body is None:
                fail(f"table '{name}': braces unbalanced in {rel}")
            decls.append((rel, name, tag, False, shape, body, False))
    for (rel, name, sname, star, shape, body, scalar) in decls:
        sym = elf.find(name, hint=rel)
        if sym is None:
            if scalar:
                # Not in the ELF: stays compiled; the compile gate fails
                # on any slot reference (fail-closed discovery loop).
                continue
            segs = split_top_level(body)
            toks = set()
            for seg in segs:
                toks.update(segment_label_tokens(seg, known))
            if not toks:
                continue
            # Recomp-only table (no qualified ELF slice exists): the
            # source is the authority for the layout, the payloads are
            # still the provenance-verified label resources. Struct rows
            # keep their type; mixed rows get a per-row live-fill /
            # compiled-constant split in analyze.
            fams = {known[t][1] for t in toks}
            if not (fams & LIVE_FAMILIES):
                continue  # all deferred: stays compiled
            t = TextTable(name, rel, sname, star, shape, b"", body)
            t.source_only = True
            t.kind = "text"
            tables.append(t)
            continue
        if sym[2] == 0 or sym[2] % 4:
            fail(f"table '{name}': ELF size {sym[2]} not a multiple "
                 f"of 4")
        t = TextTable(name, rel, sname, star, shape,
                      elf.slice(name, sym[2], hint=rel), body)
        t.scalar = scalar
        raw.append(t)
    # Pass 1: label words -> text tables (all LIVE or all deferred).
    # Pure pointer tables must resolve EVERY word: a foreign word is a
    # pointer to a non-inventory string (e.g. the Japanese
    # gStatusConditionString_* column of gStatusConditionStringsTable in
    # battle_main.c) and the table must stay compiled. Struct-value
    # tables carry scalar words by design; only their label words
    # classify them.
    for t in raw:
        labels = {w for w in t.words if w in addr_of}
        if not labels:
            # No label words -> not a text table. Engine structs
            # (sWindowTemplates, subsprite/sprite tables) carry only
            # scalar words and stay compiled outside the R13-C scope,
            # unrecorded. A struct table that reaches analyze with
            # label words but whose struct has no pointer fields is
            # still caught by the no-ptr rule there.
            continue
        fams = {known[addr_of[w]][1] for w in labels}
        live = fams & LIVE_FAMILIES
        pure = not (t.sname and not t.star)
        if pure and any(w != 0 and w not in addr_of for w in t.words):
            if live:
                # Foreign pointer words with live rows (e.g. the Japanese
                # column of gStatusConditionStringsTable): migrate; analyze
                # cuts per-row, leaving foreign words as compiled-constant
                # rows. All-foreign pure tables stay compiled (they never
                # reference slots and carry no inventory payload).
                t.kind = "text"
                tables.append(t)
            continue
        if live and not (fams - LIVE_FAMILIES):
            t.kind = "text"
            tables.append(t)
        elif not live:
            continue  # deferred families: stays compiled
        else:
            # Mixed live/deferred family labels (e.g. the exchange-corner
            # menus: map-dialogue asm rows plus a shared gText_Exit row):
            # migrate; analyze resolves every row as a fill or a compiled
            # constant — live-family rows are never compiled (§17).
            t.kind = "text"
            tables.append(t)
    # Pass 2 (iterative): pointer tables whose words are other cut
    # tables' addresses -> sub-table rows (kind-2 fills). Every word must
    # resolve (labels/tables/NULL); struct-value tables and anything
    # with foreign words stay compiled.
    table_addr = {t.name for t in tables}
    while True:
        added = False
        for t in raw:
            if t in tables or (t.sname and not t.star) or not t.words:
                continue
            if len(t.shape) > 1:
                # Multi-dim pointer tables are not 1-D subtables; pass 1
                # already cut the all-label ones, so any remaining one has
                # table-address (or mixed) words and belongs in analyze's
                # star/pure branch, which derives the inner width and
                # defers rows that do not resolve (e.g.
                # sTourneyTreeLineSections pointing at engine tables).
                tables.append(t)
                added = True
                continue
            label_fams = {known[addr_of[w]][1]
                          for w in t.words if w in addr_of}
            if all(w == 0 or w in addr_of or
                   (w in by_addr and by_addr[w] in table_addr)
                   for w in t.words) \
                    and any(w in by_addr and by_addr[w] in table_addr
                            for w in t.words) \
                    and not (label_fams - LIVE_FAMILIES):
                t.kind = "subtable"
                tables.append(t)
                added = True
        if not added:
            break
    # Any table left unclassified that still carries live inventory
    # labels stays compiled (foreign rows or a deferred-family mix);
    # recorded so the deferral is documented, never silent.
    cut = {id(t) for t in tables}
    seen = set()
    for t in raw:
        if id(t) in cut or t.name in seen:
            continue
        labels = {w for w in t.words if w in addr_of}
        if not labels:
            continue
        fams = {known[addr_of[w]][1] for w in labels}
        if fams & LIVE_FAMILIES:
            deferred.append((t.rel, t.name, sorted(fams)))
            seen.add(t.name)
    # gText_123Dot: a 9-byte `_()` label (3 rows x 3 B) that the slot
    # model cannot represent — frontier_util.c indexes it
    # (gText_123Dot[position]), and a pointer slot would make that a u8.
    # It becomes a 3-row skeleton pointer table whose kind-0 fills carry
    # byte offsets into the label resource (0/3/6); the seam adds
    # fill->row to the label base. The record must exist and be 9 bytes
    # (fail-closed), and emit_slots excludes it from the slot list.
    if "gText_123Dot" in known:
        t = TextTable("gText_123Dot", known["gText_123Dot"][2], "",
                      False, [], b"", "")
        t.kind = "text"
        tables.append(t)
    return tables, deferred


def _fill_for(t, w, addr_of, by_addr, keymap, known_records,
              table_addr, word_table=None):
    """(fill_kind, name) for one table word, or None for NULL."""
    if w == 0:
        return None
    if w in addr_of:
        rec = known_records[addr_of[w]]
        if rec[3] == "c":
            return (0, keymap[rec[0]])
        return (1, canonical(rec[0]))
    name = None
    if word_table is not None:
        name = word_table(w)
    elif w in by_addr and by_addr[w] in table_addr:
        name = by_addr[w]
    if name:
        return (2, name)
    fail(f"table '{t.name}': word 0x{w:08x} is neither a known text "
         f"label nor a cut table address")


def analyze_text_tables(tables, records, keymap, elf, root):
    """Fills + struct initializers + source/ELF row verification for
    every discovered table (fail-closed per §10). Returns extra
    (rel, name, families, reason) tuples for tables that turned out not
    to be cuttable (recomp rewrites referencing out-of-contract labels),
    which the caller merges into the deferred report."""
    known_records = {r[0]: r for r in records}
    known_labels = set(known_records)
    addr_of = {r[5] + GEN3_GBA_ROM_BASE: r[0] for r in records}
    by_addr = {}
    sym_spans = []  # (start, end, name): non-local symbols with size,
                    # for interior-address lookups — by_addr only holds
                    # symbol starts, but struct rows can point INSIDE a
                    # native array (gTypeNames[TYPE_X] = base + i*7).
    for sym in elf.symbols:
        if sym[0] and not sym[0].startswith(".") and sym[3] & 0xF != 0 \
                and sym[1] != 0 and sym[2] != 0:
            sym_spans.append((sym[1], sym[1] + sym[2], sym[0]))
            by_addr.setdefault(sym[1], sym[0])

    def native_sym(w):
        """Non-inventory native symbol whose span contains `w` (a
        symbol start, or an interior byte address of a larger array).
        None when no symbol covers the word (fail-closed)."""
        if w in by_addr:
            return by_addr[w]
        for start, end, name in sym_spans:
            if start <= w < end:
                return name
        return None
    table_addr = {t.name for t in tables}
    table_base = {}
    for addr, name in by_addr.items():
        if name in table_addr and name not in table_base:
            table_base[name] = addr
    # Cut tables' address spans (base..base + words*4) so words that
    # point INSIDE a cut table (e.g. sDexSearchTypeOptions rows at
    # gTypeNames[TYPE_NORMAL], the address of gTypeNames[8]) resolve to
    # a kind-2 fill with the right row index.
    table_span = {t.name: (table_base[t.name], len(t.words))
                  for t in tables
                  if t.name in table_base and t.words}

    def word_table(w):
        """Cut table whose row range contains `w`, or None."""
        if w in by_addr and by_addr[w] in table_addr:
            return by_addr[w]
        for name, (base, rows) in table_span.items():
            if base <= w < base + rows * 4:
                return name
        return None

    def fill_row(w, f):
        """Row index into the target table a kind-2 fill points at
        ((w - tableBase) / 4); 0 for label fills."""
        if f[0] != 2:
            return 0
        base = table_base.get(f[1])
        if base is None:
            fail(f"kind-2 fill word 0x{w:08x} has no cut-table base")
        if (w - base) % 4:
            fail(f"kind-2 fill word 0x{w:08x} is not row-aligned in "
                 f"'{f[1]}'")
        return (w - base) // 4

    deferred_out = []
    # Constant-definition pool for designated-row / compiled-field index
    # evaluation (include/constants/*.h: numeric defines + enum members).
    pool = sorted(str(p) for p in (root / "include/constants").glob("**/*.h"))
    pool_texts = [strip_comments(open(p, errors="ignore").read())
                  for p in pool]
    for t in tables:
        if t.name == "gText_123Dot":
            # 9-byte label resource = 3 rows x 3 B ("1."/"2."/"3."); the
            # fills carry byte offsets into the label (0/3/6) and the
            # seam adds fill->row to the label base.
            rec = known_records.get("gText_123Dot")
            if rec is None or rec[3] != "c":
                fail("gText_123Dot record missing (slot exclusion)")
            # resolve() overrides the first-row run scan for this label:
            # the record spans the full 9-byte table (ELF-verified above
            # in resolve(), re-verified here), so the slice fills' byte
            # offsets 0/3/6 all land inside one payload and the seam's
            # base + fill->row stays in-record (both checks fail-closed).
            sym = elf.find("gText_123Dot")
            if sym is None or sym[2] != 9:
                fail(f"gText_123Dot ELF span "
                     f"{sym[2] if sym else '?'} != 9")
            if rec[6] != 9:
                fail(f"gText_123Dot record size {rec[6]} != 9 "
                     f"(resolve() span override)")
            t.rows = 3
            t.inner = 1
            t.fills = [(f"&gText_123Dot[{i}]", 0, keymap["gText_123Dot"],
                        i * 3) for i in range(3)]
            continue
        segs = split_top_level(t.body)
        fills = []
        if t.source_only:
            if t.star or len(t.shape) > 1:
                fail(f"recomp-only table '{t.name}': only pure 1-D "
                     f"pointer tables and struct rows are supported")
            texts = _tu_texts(root, t.rel)
            if t.sname:
                # Struct rows from source authority (the recomp renamed
                # the table, so no qualified ELF slice exists): pointer
                # fields become fills, scalar fields are evaluated from
                # source constants (e.g. sMessages_pokemon_storage_system
                # rows {gText_X, MSG_VAR_NONE}).
                header = STRUCT_HEADER.get(t.sname)
                if header is None:
                    fail(f"table '{t.name}': struct '{t.sname}' is "
                         f"file-local; move it to a header (STRUCT_HEADER)")
                hp = root / "include" / header
                if not hp.exists():
                    hp = root / header
                texts.append(open(hp, errors="ignore").read())
                fields = None
                enum_sources = [strip_comments(tx) for tx in texts]
                for txt in texts:
                    try:
                        fields = parse_struct_fields(
                            strip_comments(txt), t.sname,
                            enum_sources=enum_sources)
                    except ValueError:
                        continue
                    if fields is not None:
                        break
                if fields is None:
                    fail(f"table '{t.name}': struct '{t.sname}' "
                         f"definition not found")
                _, layout, gba_size = fields
                t.gba_size = gba_size
                init_rows = []
                used = set()
                for k, seg in enumerate(segs):
                    idx, rest = _row_designator(seg, texts, pool_texts)
                    if idx is None:
                        idx = k
                    if idx in used:
                        fail(f"table '{t.name}': duplicate row index "
                             f"{idx}")
                    used.add(idx)
                    fvals = _field_values(rest, layout)
                    parts = []
                    row_fills = []
                    for (kind, fname, count, bits, off, size, extra,
                         bitpos) in layout:
                        if fname not in fvals:
                            fail(f"table '{t.name}' row {idx}: no source "
                                 f"value for field '{fname}'")
                        src = fvals[fname]
                        if kind in ("ptr", "ptrs"):
                            toks = segment_label_tokens(src, known_labels)
                            if count != 1 or len(toks) != 1:
                                fail(f"table '{t.name}' row {idx}: "
                                     f"pointer field '{fname}' must be "
                                     f"exactly one known label")
                            rec = known_records[toks[0]]
                            f = ((0, keymap[toks[0]]) if rec[3] == "c"
                                 else (1, canonical(toks[0])))
                            row_fills.append(
                                (f"&{t.name}[{idx}].{fname}", f[0], f[1],
                                 0))
                            parts.append(".%s = NULL" % fname)
                        elif kind == "scalar":
                            parts.append(".%s = %s"
                                         % (fname, fmt_scalar(
                                             extra, _eval_int_expr(
                                                 src, texts, pool_texts),
                                             size)))
                        else:
                            fail(f"table '{t.name}' row {idx}: field "
                                 f"'{fname}' kind '{kind}' not supported "
                                 f"for source-authority struct rows")
                    init_rows.append("{ %s }," % ", ".join(parts))
                    fills.extend(row_fills)
                t.rows = max(used) + 1 if used else 0
                t.init_rows = init_rows
                t.fills = fills
                continue
            for i, seg in enumerate(segs):
                toks = segment_label_tokens(seg, known_labels)
                if len(toks) != 1:
                    fail(f"recomp-only table '{t.name}' row {i}: "
                         f"{len(toks)} labels; one per row required")
                rec = known_records[toks[0]]
                f = ((0, keymap[toks[0]]) if rec[3] == "c"
                     else (1, canonical(toks[0])))
                idx, _ = _row_designator(seg, texts, pool_texts)
                fills.append((f"&{t.name}[{idx if idx is not None else i}]",
                              f[0], f[1], 0))
            t.fills = fills
            t.rows = len(segs)
            continue
        if t.kind == "subtable":
            t.rows = len(t.words)
            if len(t.words) != len(segs):
                fail(f"table '{t.name}': {len(segs)} source rows != "
                     f"{len(t.words)} ELF rows")
            for i, w in enumerate(t.words):
                f = _fill_for(t, w, addr_of, by_addr, keymap,
                              known_records, table_addr, word_table)
                if f:
                    fills.append((f"&{t.name}[{i}]", f[0], f[1],
                                  fill_row(w, f)))
            for k, seg in enumerate(segs):
                toks = sorted(segment_label_tokens(seg, known_labels))
                elfs = sorted(addr_of[w] for w in (t.words[k],)
                              if w in addr_of)
                if toks != elfs:
                    fail(f"table '{t.name}' row {k}: source tokens "
                         f"{toks} != ELF labels {elfs}")
            t.fills = fills
            continue
        # text tables
        inner = 1
        if t.sname and not t.star:
            if t.scalar:
                # Scalar struct object: the whole initializer is one
                # row (the field segments split top-level commas, so a
                # split would look like rows). The row loop runs once
                # and verifies the whole object's label tokens.
                segs = [t.body]
            # No label words -> cannot be a text table (engine struct
            # such as sWindowTemplates / sFlyingSandSubsprites); record
            # it and stay compiled without needing the struct parse.
            if not any(w in addr_of for w in t.words):
                deferred_out.append(
                    (t.rel, t.name, [],
                     "no text pointer words (engine struct table)"))
                t.gba_size = 0
                t._dropped = True
                continue
            # struct rows
            header = STRUCT_HEADER.get(t.sname)
            texts = _tu_texts(root, t.rel)
            if header:
                hp = root / "include" / header
                if not hp.exists():
                    hp = root / header  # src-relative struct definitions
                texts.append(open(hp, errors="ignore").read())
            # Object/function macros in the same texts (e.g. MCFLAVOR)
            # for row verification.
            macro_defs = _macro_defs(
                [strip_comments(tx) for tx in texts])
            fields = None
            enum_sources = [strip_comments(tx) for tx in texts]
            for txt in texts:
                try:
                    fields = parse_struct_fields(
                        strip_comments(txt), t.sname,
                        enum_sources=enum_sources)
                except ValueError:
                    continue
                if fields is not None:
                    break
            if fields is None:
                fail(f"table '{t.name}': struct '{t.sname}' definition "
                     f"not found in {t.rel} or its header")
            _, layout, gba_size = fields
            t.gba_size = gba_size
            if not any(k in ("ptr", "ptrs") for k, *_rest in layout):
                # Engine struct with no text pointer fields: every word is
                # scalar (e.g. sWindowTemplates rows are u8/u16 window
                # geometry). Not a text table — nothing to convert; stays
                # compiled and is recorded so the deferral is audited.
                deferred_out.append(
                    (t.rel, t.name, [],
                     f"engine struct {t.sname} has no text pointer fields"))
                t.gba_size = 0
                t._dropped = True
                continue
            if len(t.bytes) % gba_size:
                fail(f"table '{t.name}': {len(t.bytes)} bytes not a "
                     f"multiple of struct {t.sname} size {gba_size}")
            rows = len(t.bytes) // gba_size
            t.rows = rows
            if rows < len(segs):
                fail(f"table '{t.name}': {len(segs)} source rows > "
                     f"{rows} ELF rows")
            # A struct table with no resolvable pointer word (no label or
            # cut-table address) stays compiled: it carries no inventory
            # payload to cut and its rows never reference slots.
            if not any(w and (w in addr_of or word_table(w))
                       for w in t.words):
                deferred_out.append(
                    (t.rel, t.name, [],
                     "struct pointer fields reference only compiled "
                     "constants"))
                t.gba_size = 0
                t._dropped = True
                continue
            # Struct pointer fields must resolve to inventory labels, cut
            # tables, or non-inventory native symbols (compiled-constant
            # fields, e.g. sDexSearchTypeOptions rows pointing into
            # gTypeNames, sRegisterForTradeListMenuItems rows at sText_*).
            # A word with no symbol at all stays compiled (fail-closed).
            for row in range(rows):
                base = row * gba_size
                for (kind, fname, count, bits, off, size, extra,
                     bitpos) in layout:
                    if kind not in ("ptr", "ptrs"):
                        continue
                    for j in range(count):
                        w = t.words[(base + off) // 4 + j]
                        if w and w not in addr_of \
                                and word_table(w) is None \
                                and native_sym(w) is None:
                            deferred_out.append(
                                (t.rel, t.name, [],
                                 f"struct pointer field '{fname}' "
                                 f"references non-inventory table "
                                 f"'{hex(w)}'"))
                            t.gba_size = 0
                            t._dropped = True
                            break
                    if t.gba_size == 0:
                        break
                if t.gba_size == 0:
                    break
            if t.gba_size == 0:
                continue
            externs = {}
            init_rows = []
            compiled_parts = {}
            for row in range(rows):
                base = row * gba_size
                parts = []
                row_fills = []
                for (kind, fname, count, bits, off, size, extra,
                     bitpos) in layout:
                    if kind in ("ptr", "ptrs"):
                        vals = []
                        for j in range(count):
                            w = t.words[(base + off) // 4 + j]
                            if w and w not in addr_of \
                                    and word_table(w) is None:
                                # Compiled-constant field (non-inventory
                                # native symbol): the field keeps its
                                # compiled payload via extern; live-family
                                # rows are never compiled (§17) — words
                                # without a symbol were deferred above.
                                sym = native_sym(w)
                                if count > 1:
                                    fail(f"table '{t.name}' row {row}: "
                                         f"compiled pointer field "
                                         f"'{fname}[{j}]' (multi) not "
                                         f"supported")
                                try:
                                    expr = _field_value(
                                        segs[row], fname, layout, texts,
                                        pool_texts, sym)
                                except ValueError as e:
                                    fail(f"table '{t.name}' row {row}: {e}")
                                compiled_parts.setdefault(
                                    row, {})[fname] = expr
                                t.compiled_externs.setdefault(
                                    sym, _decl_full(texts, sym)
                                    or ("const u8", "[]"))
                                vals.append("NULL")
                                continue
                            f = _fill_for(t, w, addr_of, by_addr,
                                          keymap, known_records,
                                          table_addr, word_table)
                            if f is None:
                                vals.append("NULL")
                            else:
                                vals.append("NULL")
                                if t.scalar:
                                    target = f"&{t.name}.{fname}"
                                elif count > 1:
                                    target = (f"&{t.name}[{row}].{fname}"
                                              f"[{j}]")
                                else:
                                    target = f"&{t.name}[{row}].{fname}"
                                row_fills.append((target, f[0], f[1],
                                                  fill_row(w, f)))
                        if compiled_parts.get(row, {}).get(fname) is not None:
                            parts.append(".%s = %s"
                                         % (fname,
                                            compiled_parts[row][fname]))
                        elif count > 1:
                            parts.append(".%s = { %s }"
                                         % (fname, ", ".join(vals)))
                        else:
                            parts.append(".%s = %s" % (fname, vals[0]))
                    elif kind == "func":
                        w = t.words[(base + off) // 4]
                        if w == 0:
                            parts.append(".%s = { NULL }" % fname)
                        elif w in by_addr:
                            fn = by_addr[w]
                            if fn not in externs:
                                externs[fn] = (extra[0][0], fn,
                                               extra[0][2])
                            parts.append(".%s = { .%s = &%s }"
                                         % (fname, extra[0][1], fn))
                        else:
                            fail(f"table '{t.name}' row {row}: function "
                                 f"word 0x{w:08x} has no ELF symbol")
                    elif kind == "scalar":
                        val = int.from_bytes(
                            t.bytes[base + off:base + off + size],
                            "little")
                        parts.append(".%s = %s"
                                     % (fname, fmt_scalar(extra, val,
                                                          size)))
                    elif kind == "scalars":
                        vals = []
                        for j in range(count):
                            b = t.bytes[base + off + j]
                            vals.append(fmt_scalar(extra, b, 1))
                        parts.append(".%s = { %s }"
                                     % (fname, ", ".join(vals)))
                    elif kind == "bit":
                        b = t.bytes[base + off]
                        parts.append(".%s = 0x%02x"
                                     % (fname, (b >> bitpos)
                                        & ((1 << bits) - 1)))
                if t.scalar:
                    # Scalar object: emit the fields bare (the host
                    # definition adds the braces).
                    init_rows.append(", ".join(parts))
                else:
                    init_rows.append("{ %s }," % ", ".join(parts))
                fills.extend(row_fills)
                # row verification: source row k vs label words of row k
                if row < len(segs):
                    toks = sorted(
                        segment_label_tokens(segs[row], known_labels))
                    elfs = sorted(
                        addr_of[w] for (kind, fname, count, bits, off,
                                        size, extra, bitpos) in layout
                        if kind in ("ptr", "ptrs")
                        for j in range(count)
                        for w in [t.words[(base + off) // 4 + j]]
                        if w in addr_of)
                    if toks != elfs:
                        # Macro-initializer rows (e.g. `.flavorTexts =
                        # MCFLAVOR(Brendan)`) carry no direct labels;
                        # expand their calls (## token pasting handled)
                        # and re-verify against the ELF. Recorded in the
                        # report for documentation.
                        try:
                            exp = expand_macro_calls(segs[row],
                                                     macro_defs)
                        except ValueError as e:
                            fail(f"table '{t.name}' row {row}: {e}")
                        ntoks = sorted(segment_label_tokens(
                            exp, known_labels))
                        if ntoks == elfs:
                            t.macros.append((row, segs[row].strip()))
                        else:
                            fail(f"table '{t.name}' row {row}: source "
                                 f"tokens {toks} (expanded {ntoks}) "
                                 f"!= ELF labels {elfs}")
            # padded rows (declared size): every word must be zero
            for row in range(len(segs), rows):
                if any(t.words[row * gba_size // 4:
                               (row + 1) * gba_size // 4]):
                    fail(f"table '{t.name}': padded row {row} is not "
                         f"all-NULL")
            t.init_rows = init_rows
            t.externs = sorted(externs.values())
            t.fills = fills
            continue
        # pure pointer tables (1D or [][N]) and `struct T *const` rows
        # Object/function macros in the TU (e.g. MCFLAVOR in pokenav.h
        # for gMatchCallFlavorTexts) for row verification, same
        # include-resolution as the struct branch.
        texts = _tu_texts(root, t.rel)
        macro_defs = _macro_defs(
            [strip_comments(tx) for tx in texts])
        inner = 1
        for b in t.shape[1:]:
            b = b.strip()
            if not re.match(r"^\d+$", b):
                # Non-numeric inner bound (e.g. a macro): derive it from
                # the source row count — the ELF slice must be an exact
                # multiple. Any designator gap/row-count lie is caught by
                # the per-row label verification below.
                if not segs or len(t.words) % len(segs):
                    fail(f"table '{t.name}': inner bound '{b}' is not a "
                         f"numeric constant and cannot be derived from "
                         f"{len(segs)} source rows / {len(t.words)} words")
                inner = len(t.words) // len(segs)
                break
            inner *= int(b)
        if len(t.words) % inner:
            fail(f"table '{t.name}': {len(t.words)} words not divisible "
                 f"by inner width {inner}")
        elf_rows = len(t.words) // inner
        rows = elf_rows
        if elf_rows < len(segs):
            if len(t.shape) > 1 or t.sname or t.star:
                fail(f"table '{t.name}': {len(segs)} source rows > "
                     f"{elf_rows} ELF rows")
            # Source rows beyond the qualified slice: the recomp source may
            # extend a pret table (appended rows) or rewrite it (a row
            # replaced mid-table, e.g. the recomp option menu inserts
            # MENUITEM_DISPLAY / gText_DisplaySettings, an out-of-contract
            # label). Only a compatible extension can be cut: every
            # overlapping row must match the ELF slice, and every extra
            # row must be a single known inventory label. Anything else
            # stays compiled (deferred) — its inventory rows resolve
            # through the slots.
            rows = len(segs)
        t.rows = rows
        t.inner = inner
        # Every word must resolve to a label, a cut table, or a
        # non-inventory native symbol (compiled-constant row — e.g. the
        # Japanese column of gStatusConditionStringsTable, the sText_*
        # rows of trade.h's sMessages). A word with no symbol at all is
        # not a text table (e.g. sTourneyTreeLineSections rows at
        # scalar line-section tables) and stays compiled, recorded with
        # the first offending symbol.
        if not any(w in addr_of or word_table(w)
                   for w in t.words):
            # No resolvable rows at all: every pointer is a compiled
            # constant — no inventory payload to cut, no slot
            # references; stays compiled (all-foreign tables).
            bad = next((w for w in t.words if w and w not in addr_of
                        and not (w in by_addr
                                 and by_addr[w] in table_addr)), None)
            sym = by_addr.get(bad) if bad is not None else None
            if sym and sym in known_labels:
                target = f"out-of-scope label '{sym}'"
            elif sym:
                target = f"non-inventory symbol '{sym}'"
            else:
                target = hex(bad) if bad is not None else "none"
            deferred_out.append(
                (t.rel, t.name, [], f"pointer rows reference {target}"))
            t._dropped = True
            continue
        defer_table = False
        compiled = {}     # row -> {j: compiled symbol}
        fill_labels = set()
        for row in range(rows):
            fill_start = len(fills)  # rewrite exchanges re-roll this row
            for j in range(inner):
                i = row * inner + j
                if i >= elf_rows * inner:
                    # source-only extended row: one known label token, or
                    # one compiled-constant symbol (recomp-added row, e.g.
                    # option_menu.c's MENUITEM_DISPLAY).
                    toks = segment_label_tokens(segs[row], known_labels)
                    try:
                        idx, rest = _row_designator(segs[row], texts,
                                                    pool_texts)
                    except ValueError as e:
                        fail(f"table '{t.name}' row {row}: {e}")
                    if idx is None:
                        idx = row
                    if len(toks) == 1:
                        rec = known_records[toks[0]]
                        f = ((0, keymap[toks[0]]) if rec[3] == "c"
                             else (1, canonical(toks[0])))
                        target = (f"&{t.name}[{idx}][{j}]" if inner > 1
                                  else f"&{t.name}[{idx}]")
                        fills.append((target, f[0], f[1], 0))
                        fill_labels.add(toks[0])
                        continue
                    syms = LABEL_TOKEN_RE.findall(rest)
                    if len(toks) == 0 and len(syms) == 1 \
                            and _decl_full(texts, syms[0]) is not None:
                        compiled.setdefault(idx, {})[j] = syms[0]
                        t.compiled_externs.setdefault(
                            syms[0], _decl_full(texts, syms[0])
                            or ("const u8", "[]"))
                        continue
                    deferred_out.append((t.rel, t.name,
                                         sorted({known[rec][1] for rec
                                                 in toks if rec in known}),
                                         "recomp-only rewrite: extra "
                                         f"row {row} has {len(toks)} "
                                         f"known labels"))
                    t._dropped = True
                    defer_table = True
                    break
                w = t.words[i]
                if inner > 1:
                    target = f"&{t.name}[{row}][{j}]"
                else:
                    target = f"&{t.name}[{i}]"
                if w == 0:
                    continue
                if w in addr_of or word_table(w):
                    f = _fill_for(t, w, addr_of, by_addr, keymap,
                                  known_records, table_addr, word_table)
                    if f:
                        fills.append((target, f[0], f[1], fill_row(w, f)))
                        fill_labels.add(addr_of.get(w) or word_table(w))
                    continue
                sym = by_addr.get(w)
                if sym is None:
                    deferred_out.append(
                        (t.rel, t.name, [],
                         f"pointer row word 0x{w:08x} has no ELF "
                         f"symbol"))
                    t._dropped = True
                    defer_table = True
                    break
                # Compiled-constant word (non-inventory native symbol):
                # the row stays compiled via extern; live-family labels
                # always resolve above (§17).
                compiled.setdefault(row, {})[j] = sym
                t.compiled_externs.setdefault(
                    sym, _decl_full(texts, sym) or ("const u8", "[]"))
            if row < len(segs) and row < elf_rows:
                toks = sorted(
                    segment_label_tokens(segs[row], known_labels))
                elfs = sorted(addr_of[w] for w in t.words[
                    row * inner:(row + 1) * inner] if w in addr_of)
                if toks != elfs:
                    # Macro-initializer rows (e.g. gMatchCallFlavorTexts
                    # rows `[REMATCH_ROSE] = MCFLAVOR(AromaLady_Rose)`)
                    # carry no direct labels; expand their calls (##
                    # token pasting handled) and re-verify against the
                    # ELF. Recorded in the report for documentation.
                    try:
                        exp = expand_macro_calls(segs[row], macro_defs)
                    except ValueError as e:
                        fail(f"table '{t.name}' row {row}: {e}")
                    ntoks = sorted(segment_label_tokens(
                        exp, known_labels))
                    if ntoks == elfs:
                        t.macros.append((row, segs[row].strip()))
                        continue
                    if inner == 1 and len(toks) == 1 and len(elfs) == 1 \
                            and toks[0] != elfs[0]:
                        # Recomp rewrite exchange: the source row names a
                        # different inventory label than the ROM (trade.h
                        # sMessages row 3: gText_OnlyPkmnForBattle vs
                        # retail gText_RecordingGameResults). Publish the
                        # source's label; the replaced ROM label is
                        # recorded as discarded so the coverage check
                        # below treats it as covered (documented in the
                        # deferred/rewrite report).
                        rec = known_records[toks[0]]
                        f = ((0, keymap[toks[0]]) if rec[3] == "c"
                             else (1, canonical(toks[0])))
                        del fills[fill_start:]
                        fills.append((f"&{t.name}[{row}]", f[0], f[1], 0))
                        fill_labels.add(toks[0])
                        t.rewrite_discards.update(elfs)
                        continue
                    if inner == 1 and not toks:
                        # Recomp rewrite: the row's value is a compiled
                        # constant (option_menu.c inserts
                        # gText_DisplaySettings at MENUITEM_DISPLAY); the
                        # ELF labels of this row are filled from a later
                        # source row (enforced by the inclusion check
                        # below).
                        try:
                            idx, rest = _row_designator(
                                segs[row], texts, pool_texts)
                        except ValueError as e:
                            fail(f"table '{t.name}' row {row}: {e}")
                        syms = LABEL_TOKEN_RE.findall(rest)
                        if len(syms) == 1 \
                                and _decl_full(texts, syms[0]) \
                                is not None:
                            compiled.setdefault(
                                idx if idx is not None else row,
                                {})[0] = syms[0]
                            t.compiled_externs.setdefault(
                                syms[0], _decl_full(texts, syms[0])
                                or ("const u8", "[]"))
                            continue
                    deferred_out.append((t.rel, t.name,
                                         sorted({known_records[
                                                     addr_of[w]][1]
                                                 for w in t.words[
                                                     row * inner:
                                                     (row + 1) * inner]
                                                 if w in addr_of}),
                                         "recomp-only rewrite: row "
                                         f"{row} source {toks} (expanded "
                                         f"{ntoks}) != ELF {elfs}"))
                    t._dropped = True
                    defer_table = True
                    break
            if defer_table:
                break
        if defer_table:
            continue
        # Every ELF label must be filled from some source row, or be
        # explicitly discarded by a recomp rewrite exchange: a rewrite
        # must not drop ROM labels silently.
        elf_labels = {addr_of[w] for w in t.words if w in addr_of}
        if not elf_labels <= (fill_labels | t.rewrite_discards):
            fail(f"table '{t.name}': rewrite left ELF labels "
                 f"uncovered: "
                 f"{sorted(elf_labels - fill_labels - t.rewrite_discards)}")
        for row in range(len(segs), rows):
            if any(t.words[row * inner:(row + 1) * inner]):
                fail(f"table '{t.name}': padded row {row} is not "
                     f"all-NULL")
        if compiled:
            init_rows = []
            for row in sorted(compiled):
                if inner > 1:
                    cols = [compiled[row].get(j, "NULL")
                            for j in range(inner)]
                    init_rows.append("[%d] = { %s },"
                                     % (row, ", ".join(cols)))
                else:
                    init_rows.append("[%d] = %s,"
                                     % (row, compiled[row].get(0, "NULL")))
            t.init_rows = init_rows
        t.fills = fills
    # Deferred tables must not reach skeleton emission (their struct
    # initializers/rows are intentionally incomplete). Transitive rule:
    # sub-table fills whose target table ended up deferred (engine
    # structs, foreign pointers) have no host array to fill from, so
    # the referencing table must defer too (e.g.
    # sTourneyTreeLineSections rows at scalar line-section tables).
    dropped_names = {t.name for t in tables if t._dropped}
    while True:
        changed = False
        for t in tables:
            if t._dropped:
                continue
            for (_, kind, fname, _row) in t.fills:
                if kind == 2 and fname in dropped_names:
                    deferred_out.append(
                        (t.rel, t.name, [],
                         f"sub-table rows reference deferred table "
                         f"'{fname}'"))
                    t._dropped = True
                    changed = True
                    break
        if not changed:
            break
        dropped_names = {t.name for t in tables if t._dropped}
    tables[:] = [t for t in tables if not t._dropped]
    # File-local tables can share a name across TUs (sMessages in
    # berry_crush.c vs pokemon_storage_system.c, sRecordsTexts in
    # dodrio_berry_picking.c vs pokemon_jump.c). The generated host
    # arrays all live in one object file, so colliding names get a
    # deterministic TU-derived suffix; the cutover updates the native
    # referencing TU (R13-C §10 "where architecture permits").
    by_name = {}
    for t in tables:
        by_name.setdefault(t.name, []).append(t)
    for name, occ in by_name.items():
        if len(occ) == 1:
            continue
        for t in occ:
            host = f"{name}_{Path(t.rel).stem}"
            if any(other is not t and other.host_name == host
                   for other in occ):
                fail(f"table '{t.name}': host-name collision for "
                     f"'{host}'")
            t.host_name = host
    for t in tables:
        new_fills = []
        for (target, kind, fname, row) in t.fills:
            if kind == 2:
                occ = by_name.get(fname, [])
                if len(occ) > 1:
                    fail(f"table '{t.name}': kind-2 fill targets "
                         f"colliding table '{fname}' — disambiguation "
                         f"would be ambiguous")
                if occ:
                    fname = occ[0].host_name
            new_fills.append(
                (target.replace(f"&{t.name}[", f"&{t.host_name}["),
                 kind, fname, row))
        t.fills = new_fills
    return deferred_out


# ------------------------------------------------------------- artifacts
def bundle_key(kind, path):
    """Bundle resource id for an asm file (sorted for determinism)."""
    p = path.replace("\\", "/")
    if p.startswith("data/maps/"):
        parts = p.split("/")
        map_name = parts[2]
        return f"emerald:text/map/{canonical(map_name)}"
    if p.startswith("data/text/"):
        stem = p[len("data/text/"):]
        if stem.endswith(".inc"):
            stem = stem[:-4]
        return f"emerald:text/data/{canonical(stem)}"
    if p.startswith("data/scripts/"):
        stem = p[len("data/scripts/"):]
        if stem.endswith(".inc"):
            stem = stem[:-4]
        return f"emerald:text/scripts/{canonical(stem)}"
    if p == "data/event_scripts.s":
        return "emerald:text/scripts/event-scripts"
    return f"emerald:text/data/{canonical(Path(p).stem)}"


def bundle_blob(labels):
    """Self-describing blob: u32 count, u32 dataOffset, u32 offsets[count],
    then the canonical bytes of each label in canonical-name order."""
    ordered = sorted(labels, key=lambda t: canonical(t[0]))
    data = b"".join(b for _, _, b in ordered)
    count = len(ordered)
    data_off = 8 + 4 * count
    offs = [data_off]
    for _, _, b in ordered[:-1]:
        offs.append(offs[-1] + len(b))
    header = struct.pack("<II", count, data_off)
    return header + struct.pack("<%dI" % count, *offs) + data


# ------------------------------------------------------------- emission
def write_if(path, lines, args):
    text = "\n".join(lines)
    if args.check:
        if not path.exists():
            fail(f"--check: {path} does not exist")
        if path.read_text() != text:
            fail(f"--check: {path} differs from deterministic regeneration")
        print(f"check passed: {path}")
    else:
        path.write_text(text)
        print(f"wrote {path}")


def emit_artifacts(records, fam_dir, args, rom_bytes, keymap):
    """Per-label .bin artifacts + bundle blobs. Returns (label_arts,
    bundle_blobs) as {id: rel path}."""
    label_arts = {}
    for r in records:
        if r[3] != "c":
            continue
        label, fam, path, kind, section, off, size = r
        # Artifact filename = the resource key's last segment (== canonical
        # for every label except the WeRe/Were collision group, where the
        # ordinal suffix keeps the two artifacts distinct).
        rid = keymap[label]
        rel = (f"resources/extraction/emerald/bpee01/text/labels/"
               f"{fam}/{rid.rsplit('/', 1)[1]}.bin")
        art = root / rel
        data = rom_bytes[off:off + size]
        if args.check:
            if not art.exists() or art.read_bytes() != data:
                fail(f"--check: {art} differs from deterministic ROM-slice "
                     f"regeneration")
        else:
            art.parent.mkdir(parents=True, exist_ok=True)
            art.write_bytes(data)
        label_arts[keymap[label]] = rel

    bundle_files = {}
    for r in records:
        if r[3] == "asm":
            bundle_files.setdefault(r[2], []).append(r)
    keys = {}
    for path, recs in sorted(bundle_files.items()):
        key = bundle_key("asm", path)
        if key in keys:
            fail(f"bundle key collision: '{key}' from {keys[key]} and {path}")
        keys[key] = path
    bundle_blob_bytes = {}
    for key, path in sorted(keys.items()):
        recs = sorted(bundle_files[path], key=lambda r: canonical(r[0]))
        labels = [(r[0], None, bytes(rom_bytes[r[5]:r[5] + r[6]]))
                  for r in recs]
        blob = bundle_blob(labels)
        rel = f"resources/extraction/emerald/bpee01/text/bundles/{key[len('emerald:text/'):]}.bin"
        art = root / rel
        if args.check:
            if not art.exists() or art.read_bytes() != blob:
                fail(f"--check: {art} differs from deterministic bundle "
                     f"regeneration")
        else:
            art.parent.mkdir(parents=True, exist_ok=True)
            art.write_bytes(blob)
        bundle_blob_bytes[key] = blob

    n_maps = sum(1 for k in keys if k.startswith("emerald:text/map/"))
    n_data = sum(1 for k in keys if k.startswith("emerald:text/data/"))
    n_scripts = sum(1 for k in keys if k.startswith("emerald:text/scripts/"))
    if n_maps != PINNED_BUNDLE_MAPS or n_data != PINNED_BUNDLE_DATA_TEXT \
            or n_scripts != PINNED_BUNDLE_SCRIPTS:
        fail(f"bundles {n_maps} maps + {n_data} data/text + {n_scripts} "
             f"scripts != pinned {PINNED_BUNDLE_MAPS} + "
             f"{PINNED_BUNDLE_DATA_TEXT} + {PINNED_BUNDLE_SCRIPTS}")
    if len(keys) != PINNED_BUNDLES:
        fail(f"bundles {len(keys)} != pinned {PINNED_BUNDLES}")
    return label_arts, bundle_blob_bytes


def emit_family(records, dual, label_arts, bundle_blobs, fam_dir, args, keymap):
    fam = fam_dir
    # inventory
    from collections import Counter
    fam_c = Counter(r[1] for r in records)
    fam_b = Counter()
    for r in records:
        fam_b[r[1]] += r[6]
    inv_lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "inventory_version = 1",
        'family = "text"',
        'game = "emerald"',
        'rom_profile = "bpee01"',
        "",
        "# Pinned counts (R13C-AC-1, authoritative):",
        f"# {PINNED_TOTAL_LABELS} labels / {PINNED_TOTAL_BYTES} B; "
        f"script_data {PINNED_SCRIPT_LABELS} / {PINNED_SCRIPT_BYTES} B;",
        f"# C-side per-label resources {PINNED_C_RESOURCES} / "
        f"{PINNED_C_RESOURCE_BYTES} B; asm-source {PINNED_ASM_LABELS} / "
        f"{PINNED_ASM_BYTES} B",
        f"# across {PINNED_BUNDLES} bundles ({PINNED_BUNDLE_MAPS} maps + "
        f"{PINNED_BUNDLE_DATA_TEXT} data/text + "
        f"{PINNED_BUNDLE_SCRIPTS} scripts).",
        "",
    ]
    for fam_name in sorted(FAMILY_PINS):
        inv_lines.append("[[families]]")
        inv_lines.append(f'name = "{fam_name}"')
        inv_lines.append(f"symbol_count = {fam_c[fam_name]}")
        inv_lines.append(f"canonical_bytes = {fam_b[fam_name]}")
        inv_lines.append("")
    write_if(fam / "inventory.generated.toml", inv_lines, args)

    # catalog (5,187 entries: type text, schema 1)
    cat_lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "catalog_version = 1",
        'namespace = "emerald"',
        'resource_api = "1.0.0"',
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    ids = sorted(set(label_arts) | set(bundle_blobs))
    for rid in ids:
        cat_lines.append("[[resources]]")
        cat_lines.append(f'id = "{rid}"')
        cat_lines.append('type = "text"')
        cat_lines.append("schema = 1")
        cat_lines.append("required_for_base = true")
        cat_lines.append("")
    write_if(fam / "catalog.generated.toml", cat_lines, args)

    # bindings
    bin_lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Semantic extraction bindings for the text family.",
        "# Per-label bindings: the pret ELF symbol name (the manifest",
        "# derives rom_offset from the qualified ELF); the artifact is the",
        "# canonical charmap bytes (raw encoding).",
        "# Bundle bindings: bundle = true - the artifact is a CONSTRUCTED",
        "# blob (no ELF symbol, no ROM slice); the manifest records it with",
        "# source_artifact and the pack builder reads the artifact file.",
        "# Bundle-local labels are published in bundles.generated.toml",
        "# (plan §2: '<bundle> :: <label>', never resource keys).",
        "",
        "bindings_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    by_id = {}
    for r in records:
        if r[3] != "c":
            continue
        rid = keymap[r[0]]
        by_id[rid] = (r[0], r[6])
    for rid in sorted(label_arts):
        symbol, size = by_id[rid]
        elf_symbol = RENAME_MAP.get(symbol, symbol)
        bin_lines.append("[[bindings]]")
        bin_lines.append(f'id = "{rid}"')
        bin_lines.append(f'symbol = "{elf_symbol}"')
        bin_lines.append(f'source_artifact = "{label_arts[rid]}"')
        bin_lines.append('source_encoding = "raw"')
        bin_lines.append('canonical_representation = "gba-charmap"')
        bin_lines.append(f"expected_decoded_size = {size}")
        bin_lines.append("")
    for rid in sorted(bundle_blobs):
        blob = bundle_blobs[rid]
        bin_lines.append("[[bindings]]")
        bin_lines.append(f'id = "{rid}"')
        bin_lines.append('symbol = ""')
        bin_lines.append('bundle = true')
        bin_lines.append('source_artifact = '
                         f'"resources/extraction/emerald/bpee01/text/bundles/'
                         f'{rid[len("emerald:text/"):]}.bin"')
        bin_lines.append('source_encoding = "raw"')
        bin_lines.append('canonical_representation = "gba-charmap"')
        bin_lines.append(f"expected_decoded_size = {len(blob)}")
        bin_lines.append("")

    write_if(fam / "bindings.generated.toml", bin_lines, args)

    # Bundle provenance: one [[bundles]] block per bundle with the sorted
    # per-label (symbol, offset, size) inventory. A SEPARATE file on purpose:
    # the shared Gen3Toml parser has no dotted-key table headers, so the
    # provenance stays out of the manifest tool's input (bindings.generated.
    # toml) and lives in its own audit-facing document.
    bundle_files = {}
    for r in records:
        if r[3] == "asm":
            bundle_files.setdefault(bundle_key("asm", r[2]), []).append(r)
    bundle_lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Bundle provenance (plan §2: '<bundle> :: <label>', never resource",
        "# keys): every bundle's constructed-blob identity plus the sorted",
        "# per-label (symbol, canonical, ROM offset, size) inventory.",
        "",
        "bundles_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    for rid in sorted(bundle_blobs):
        bundle_lines.append("[[bundles]]")
        bundle_lines.append(f'id = "{rid}"')
        bundle_lines.append('kind = "constructed-blob"')
        bundle_lines.append(f"label_count = {len(bundle_files[rid])}")
        bundle_lines.append(f"blob_size = {len(bundle_blobs[rid])}")
        for r in sorted(bundle_files[rid], key=lambda r: canonical(r[0])):
            bundle_lines.append(f"[[bundles.labels]]")
            bundle_lines.append(f'symbol = "{r[0]}"')
            bundle_lines.append(f'canonical = "{canonical(r[0])}"')
            bundle_lines.append(f"rom_offset = {r[5]}")
            bundle_lines.append(f"size = {r[6]}")
        bundle_lines.append("")
    write_if(fam / "bundles.generated.toml", bundle_lines, args)

    # ownership
    own_lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Native asset-ownership declaration. Each [[resources]] block",
        "# records the canonical id, the M0/M1 key (SHA-256 of",
        "# \"gen3-resource-id-v1\\0\" + canonical name - the pack key),",
        "# legacy compiled symbol(s), type, source artifact, per-target",
        "# ownership state and source hashes.",
        "#",
        "# R13-C states (plan §11):",
        "#  - battle/move/ability/nature (skeleton tables) and",
        "#    shared/system/match-call/ribbon (pointer slots) are the live",
        "#    cutover tranches: ROM_BASE_ONLY on native (the compiled arrays",
        "#    are removed; the seam publishes arena payloads and fills the",
        "#    generated slots/skeleton tables).",
        "#  - item/pokedex/easy-chat are table-blocked (gItems/gPokedexEntries/",
        "#    sEasyChatGroupWords repoint in R13-D/E): COMPILED_PENDING_MIGRATION.",
        "#  - all bundle resources are additive: COMPILED_PENDING_MIGRATION",
        "#    (script operands embed native addresses; R13-G re-emits them as",
        "#    GBA logical addresses). The 97 dual-referenced labels stay",
        "#    compiled through R13-G, documented in consumers.generated.toml.",
        "",
        "ownership_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    sym_fam = {r[0]: r[1] for r in records if r[3] == "c"}
    # R13-D2: the item-description family (emerald:text/item/s<item>desc)
    # flips to ROM_BASE_ONLY at that stage - native gItems[].description
    # re-points into the item text arena. R13-E3b does the SAME TARGETED flip
    # for the Pokédex description family (emerald:text/pokedex/g<species>
    # pokedextext): native gPokedexEntries[].description re-points into the
    # R13-C Pokédex text arena. Neither family is added to LIVE_FAMILIES
    # (that set also drives the slot/skeleton/table emission), so easy-chat /
    # contest etc. remain COMPILED_PENDING_MIGRATION and the text family is
    # otherwise byte-identical (only these ownership records change).
    for rid in sorted(label_arts):
        symbol, size = by_id[rid]
        label_family = sym_fam[symbol]
        state = "ROM_BASE_ONLY" if (label_family in LIVE_FAMILIES
                                    or label_family in ("item", "pokedex")) \
            else "COMPILED_PENDING_MIGRATION"
        data = root / label_arts[rid]
        h = sha256(data.read_bytes())
        own_lines.append("[[resources]]")
        own_lines.append(f'id = "{rid}"')
        own_lines.append(f'key = "{derive_key(rid)}"')
        own_lines.append(f'legacy_symbol = "{symbol}"')
        own_lines.append('type = "text"')
        own_lines.append(f'source_artifact = "{label_arts[rid]}"')
        own_lines.append(f"encoded_length = {size}")
        own_lines.append(f"decoded_length = {size}")
        own_lines.append('source_encoding = "raw"')
        own_lines.append(f'source_encoded_sha256 = "{h}"')
        own_lines.append(f'canonical_decoded_sha256 = "{h}"')
        own_lines.append(f'ownership_state = "{state}"')
        own_lines.append("")
        own_lines.append("[resources.targets]")
        own_lines.append(f'native = "{state}"')
        own_lines.append('gba = "COMPILED"')
        own_lines.append("")
    for rid in sorted(bundle_blobs):
        recs = sorted(bundle_files[rid], key=lambda r: canonical(r[0]))
        blob = bundle_blobs[rid]
        h = sha256(blob)
        own_lines.append("[[resources]]")
        own_lines.append(f'id = "{rid}"')
        own_lines.append(f'key = "{derive_key(rid)}"')
        own_lines.append('legacy_symbol = ""')
        own_lines.append('type = "text"')
        own_lines.append('bundle = true')
        own_lines.append('source_artifact = '
                         f'"resources/extraction/emerald/bpee01/text/bundles/'
                         f'{rid[len("emerald:text/"):]}.bin"')
        own_lines.append(f"encoded_length = {len(blob)}")
        own_lines.append(f"decoded_length = {len(blob)}")
        own_lines.append('source_encoding = "raw"')
        own_lines.append(f'source_encoded_sha256 = "{h}"')
        own_lines.append(f'canonical_decoded_sha256 = "{h}"')
        own_lines.append('ownership_state = "COMPILED_PENDING_MIGRATION"')
        own_lines.append("")
        own_lines.append("[resources.targets]")
        own_lines.append('native = "COMPILED_PENDING_MIGRATION"')
        own_lines.append('gba = "COMPILED"')
        own_lines.append("")
        dual_in_bundle = [r for r in recs if r[0] in dual]
        if dual_in_bundle:
            for r in dual_in_bundle:
                own_lines.append("[[resources.dual_references]]")
                own_lines.append(f'symbol = "{r[0]}"')
                own_lines.append(f'site = "{dual[r[0]]}"')
                own_lines.append('decision = "deferred-to-r13g"')
                own_lines.append(
                    'reason = "script operand + direct C code reference; '
                    'stays compiled through R13-G"')
            own_lines.append("")
    write_if(fam / "ownership.generated.toml", own_lines, args)

    # consumers
    con_lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# Do not edit by hand; re-run the generator (regeneration must be",
        "# a no-op diff).",
        "",
        "# Machine-readable consumer map for the text family (plan §5).",
        "",
        "consumers_version = 1",
        'game = "emerald"',
        'rom_profile = "bpee01-rev0"',
        "",
    ]
    sites = [
        ("table-deref", "src/battle_message.c:2243,2778",
         "gBattleStringsTable[stringID]", "live-skeleton",
         "517-slot ID-indexed table; regenerated as a host-data skeleton "
         "table filled from the battle arena (R13-C live)"),
        ("table-deref", "src/data/text/move_descriptions.h:1421",
         "gMoveDescriptionPointers", "live-skeleton",
         "354 rows; skeleton table, zero direct refs"),
        ("table-deref", "src/data/text/abilities.h",
         "gAbilityDescriptionPointers", "live-skeleton",
         "80-slot table, 78 explicit rows; skeleton table"),
        ("table-deref", "src/data/text/nature_names.h",
         "gNatureNamePointers", "live-skeleton",
         "25 rows; skeleton table"),
        ("table-deref", "src/data/contest_text_tables.h:220",
         "gContestEffectDescriptionPointers / "
         "gContestMoveTypeTextPointers", "live-skeleton",
         "rows reference bundle-local contest labels; skeleton rows fill "
         "from the contest bundle arena (blob offsets)"),
        ("table-deref", "src/data/script_menu.h:903",
         "gStdStrings", "live-skeleton",
         "30 rows of gText_* slots; skeleton table"),
        ("table-deref", "src/tv.c:353+",
         "sTV*TextGroup (32 arrays)", "live-skeleton",
         "rows reference bundle-local tv.inc labels; skeleton rows fill "
         "from the tv bundle arena"),
        ("table-deref", "src/match_call.c:753+",
         "sMatchCall*Texts (14 arrays)", "live-skeleton",
         ".text rows reference bundle-local match_call.inc labels; "
         "skeleton rows fill from the match-call bundle arena"),
        ("direct-ref", "src/strings.c + 112 objects",
         "gText_* immediates (7,330 relocations, 5,547 symbols)",
         "live-slots",
         "PrintString(gText_X)-style immediates; generated pointer slots "
         "with zero call-site edits"),
        ("direct-ref", "src/pokedex.c etc.",
         "gPokedexEntries[] + 387 direct refs", "deferred-table",
         "gItems/gPokedexEntries row repoint in R13-D/E"),
        ("direct-ref", "src/item.c etc.",
         "gItems[].description", "deferred-table",
         "struct-embedded rows repoint in R13-D"),
        ("direct-ref", "src/battle_setup.c etc.",
         "sEasyChatGroupWords blob indexing", "deferred-table",
         "compiled word blobs (R13-B-remaining leaf family) repoint with "
         "the easy-chat migration"),
        ("bytecode-operand", "src/scrcmd.c:72",
         "msgbox/showmessage/trainerbattle operands", "deferred-r13g",
         "script operands embed native addresses; R13-G re-emits them as "
         "GBA logical addresses"),
        ("bytecode-operand", "src/battle_script_commands.c",
         "T1_READ_PTR/T2_READ_PTR operands", "deferred-r13g",
         "battle-script text reads"),
    ]
    for kind, site, consumer, decision, reason in sites:
        con_lines.append("[[consumer_sites]]")
        con_lines.append(f'kind = "{kind}"')
        con_lines.append(f'site = "{site}"')
        con_lines.append(f'consumer = "{consumer}"')
        con_lines.append(f'decision = "{decision}"')
        con_lines.append(f'reason = "{reason}"')
        con_lines.append("")
    for label in sorted(dual):
        con_lines.append("[[consumer_sites]]")
        con_lines.append('kind = "c-direct-ref"')
        con_lines.append(f'site = "{dual[label]}"')
        con_lines.append(f'consumer = "{label}"')
        con_lines.append('decision = "deferred-to-r13g"')
        con_lines.append('reason = "script operand + direct C code '
                         'reference; slot conversion would corrupt the '
                         'script read"')
        con_lines.append("")
    write_if(fam / "consumers.generated.toml", con_lines, args)


def emit_seam_header(records, label_arts, bundle_blobs, root, args, keymap):
    # One globally sorted key list: the seam binary-searches
    # kTextNativeResources (FindNativeResource) and the emitted file's
    # own contract says "sorted key order". Concatenating the label and
    # bundle partitions left bundle ids like emerald:text/data/* sorting
    # AFTER later labels (emerald:text/system/gtext-zinc1bp), so every
    # data-bundle lookup failed the search (observed: TABLE_MISMATCH at
    # emerald:text/data/abnormal-weather).
    ids = sorted(set(label_arts) | set(bundle_blobs))
    count = len(ids)
    if count != PINNED_C_RESOURCES + PINNED_BUNDLES:
        fail(f"seam inventory {count} != "
             f"{PINNED_C_RESOURCES + PINNED_BUNDLES}")

    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C seam inventory for the emerald:text family. The table",
        " * enumerates every canonical text resource the production pack",
        " * publishes (4,824 per-label + 363 bundles) with its canonical id",
        " * and payload size. The seam (EmeraldTextCompat) validates the",
        " * session's pack against this exact inventory (name/size/schema",
        " * set equality) before allocating family arenas - drift is a",
        " * publish failure, never a partial publication.",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_NATIVE_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_NATIVE_GENERATED_H",
        "",
        "#include <stdbool.h>",
        "#include <stdint.h>",
        "",
        f"#define TEXT_NATIVE_RESOURCE_COUNT {count}u",
        f"#define TEXT_NATIVE_LABEL_COUNT {PINNED_C_RESOURCES}u",
        f"#define TEXT_NATIVE_BUNDLE_COUNT {PINNED_BUNDLES}u",
        "",
        "struct TextNativeResource",
        "{",
        "    const char *name;         /* canonical resource id, e.g. ",
        "                                 emerald:text/battle/s-text-win */",
        "    uint32_t size;            /* canonical payload bytes */",
        "    bool isBundle;            /* constructed blob (no ROM slice) */",
        "    uint32_t arenaIndex;      /* family arena (ARENA_KEYS order) for",
        "                                 per-label resources; 0 (unused) for",
        "                                 bundles - their labels span arenas",
        "                                 (bundle index is authoritative) */",
        "};",
        "",
        "extern const struct TextNativeResource "
        "kTextNativeResources[TEXT_NATIVE_RESOURCE_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_TEXT_NATIVE_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/text_native.generated.h",
             lines, args)

    by_id = {}
    for r in records:
        if r[3] == "c":
            by_id[keymap[r[0]]] = (r[6], ARENA_KEYS.index(arena_of(r)))
    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C seam inventory DEFINITION (sorted key order).",
        " */",
        '#include <stdbool.h>',
        '#include "emerald/resources/text_native.generated.h"',
        "",
        "const struct TextNativeResource "
        "kTextNativeResources[TEXT_NATIVE_RESOURCE_COUNT] =",
        "{",
    ]
    for rid in ids:
        if rid in by_id:
            size, arena = by_id[rid]
            c_lines.append(f'    {{"{rid}", {size}u, false, {arena}u}},')
        else:
            size = len(bundle_blobs[rid])
            c_lines.append(f'    {{"{rid}", {size}u, true, 0u}},')
    c_lines += [
        "};",
        "",
    ]
    write_if(root / "src/emerald/resources/text_native_table.generated.c",
             c_lines, args)


def emit_slots(records, root, args):
    """Pointer slots for the slot families (shared/system/match-call/ribbon
    + skeleton-family labels reachable only via tables need no slots)."""
    slots = []
    for r in sorted(records, key=lambda r: r[0]):
        if r[3] != "c":
            continue
        if r[1] in LIVE_FAMILIES and r[0] != "gText_123Dot":
            # gText_123Dot is a 3-row slice table, not a slot: its
            # frontier_util.c indexing (gText_123Dot[position]) needs a
            # pointer per 3-byte slice, filled with byte offsets.
            slots.append(r[0])
    if len(slots) != PINNED_SLOT_BINDINGS:
        fail(f"slot count {len(slots)} != pinned {PINNED_SLOT_BINDINGS}")

    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C pointer slots for the text families cut over via the slot",
        " * mechanism (shared/system/match-call/ribbon:",
        " * PrintString(gText_X)-style immediates, zero call-site edits).",
        " * Each slot is a host-data pointer named EXACTLY like the compiled",
        " * array it replaces; the seam fills it from the family arena at",
        " * publish. The GBA flavor keeps the original const u8 arrays; the",
        " * original extern declarations are NATIVE_LINUX-guarded by the",
        " * cutover (R13-C §9). Skeleton-table families (battle/move/ability/",
        " * nature) reach their labels only through generated skeleton",
        " * tables and need no slots.",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_SLOTS_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_SLOTS_GENERATED_H",
        "",
        # R13-C: the slot externs name u8* objects, so the header must be
        # self-contained (it is included at the top of strings.h and
        # battle_message.h, before any game type header). gba/types.h for
        # u8, gba/defines.h for HOST_DATA.
        '#include "gba/types.h"',
        '#include "gba/defines.h"',
        "",
        "#ifdef NATIVE_LINUX",
        "",
    ]
    header_slots = ["extern HOST_DATA const u8 *" + s + ";" for s in slots]
    lines += header_slots + [
        "",
        "#endif /* NATIVE_LINUX */",
        "",
        "#endif /* EMERALD_RESOURCES_TEXT_SLOTS_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/text_slots.generated.h",
             lines, args)

    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C pointer slot DEFINITIONS. host_data section, zero-filled;",
        " * the seam fills them at publish.",
        " */",
        '#include "emerald/resources/text_slots.generated.h"',
        "",
    ]
    for s in slots:
        c_lines.append(f"HOST_DATA const u8 *{s};")
    write_if(root / "src/emerald/resources/text_slots_table.generated.c",
             c_lines, args)


def emit_skeleton_arrays(tables, root, args):
    """Host arrays replacing the compiled cut-over tables (R13-C §10).
    Pure tables: `HOST_DATA const u8 *NAME[N] = { NULL };`. Struct
    tables: full designated C initializers re-derived from the ELF row
    bytes (constants) + fills (text pointers) + function externs."""
    headers = set()
    decls = []
    defs = []
    def shape_suffix(t):
        # Scalar objects (struct NamingScreenTemplate sX = {...}) are
        # single objects, not arrays. Fills target `&NAME[row][j]` for
        # inner > 1 (multi-dim tables such as sFooterTextOptions[3][4]),
        # so the declaration must keep the full shape, not a flat row
        # count.
        if t.scalar:
            return ""
        return f"[{t.rows}][{t.inner}]" if t.inner > 1 else f"[{t.rows}]"

    for t in tables:
        if t.sname and not t.star:
            hdr = STRUCT_HEADER.get(t.sname)
            if hdr is None:
                fail(f"table '{t.name}': struct '{t.sname}' is file-local; "
                     f"move it to a header (STRUCT_HEADER) before cutover")
            headers.add(hdr)
            # Deliberately NOT const-qualified OBJECTS. host_data holds
            # both `const u8 *` pointer arrays and these struct objects;
            # `const struct X` consts the OBJECT (unlike `const u8 *`,
            # which consts the pointee), giving it different section
            # flags and a "section type conflict" in the shared
            # host_data section (observed: sRegisterForTradeListMenuItems
            # vs gText_123Dot). The seam also writes these objects at
            # publish, so the const-qualified form would be UB on write.
            decls.append(f"extern HOST_DATA struct {t.sname} "
                         f"{t.host_name}{shape_suffix(t)};")
            defs.append(f"HOST_DATA struct {t.sname} "
                        f"{t.host_name}{shape_suffix(t)} =")
            defs.append("{")
            defs.extend(t.init_rows)
            defs.append("};")
        elif t.star:
            headers.add(STRUCT_HEADER[t.sname])
            decls.append(f"extern HOST_DATA const struct {t.sname} *"
                         f"{t.host_name}{shape_suffix(t)};")
            defs.append(f"HOST_DATA const struct {t.sname} *{t.host_name}"
                        f"[{t.rows}] = {{ NULL }};")
        else:
            decls.append(f"extern HOST_DATA const u8 *{t.host_name}"
                         f"{shape_suffix(t)};")
            defs.append(f"HOST_DATA const u8 *{t.host_name}"
                        f"{shape_suffix(t)} = {{ NULL }};")
    # Compiled-constant rows/fields (e.g. the Japanese column of
    # gStatusConditionStringsTable, sText_* rows in trade.h's sMessages,
    # sWallyMatchCallHeader.locationData): the host .c re-declares the
    # symbol with its full declaration type and shape, and includes the
    # header of any struct/typedef type it uses.
    extern_decls = []
    for t in tables:
        for sym, (typ, shape) in sorted(t.compiled_externs.items()):
            toks = typ.split()
            base = next((tk for tk in toks if tk != "const"), None)
            if base == "struct" and "struct" in toks:
                base = toks[toks.index("struct") + 1]
            hdr = STRUCT_HEADER.get(base)
            if hdr:
                headers.add(hdr)
            extern_decls.append(f"extern {typ} {sym}{shape};")
    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C §10 host arrays: one object per native text-pointer table",
        " * cut over. Every row is NULL at load; the seam fills the non-NULL",
        " * rows at publish (per-label rows from the family arenas,",
        " * bundle-local rows from the bundle blobs, sub-table rows from the",
        " * target table's own array). The compiled definitions are",
        " * NATIVE_LINUX-guarded off by the cutover; these externs replace",
        " * them for every consumer, so old compiled string symbol storage",
        " * is not required for converted labels (proven by link).",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_SKELETON_ARRAYS_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_SKELETON_ARRAYS_GENERATED_H",
        "",
        '#include "gba/defines.h"',
        '#include "global.h"',
    ]
    lines += sorted('#include "%s"' % h for h in headers)
    lines += ["", "#ifdef NATIVE_LINUX", ""]
    lines += decls
    lines += ["", "#endif /* NATIVE_LINUX */",
              "#endif /* EMERALD_RESOURCES_TEXT_SKELETON_ARRAYS_GENERATED_H"
              " */"]
    write_if(root / "include/emerald/resources/"
                    "text_skeleton_arrays.generated.h", lines, args)

    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/text_skeleton_arrays.generated.h"',
    ]
    c_lines += sorted('#include "%s"' % h for h in headers)
    for (ret, fn, params) in sorted({e for t in tables
                                     for e in t.externs},
                                    key=lambda e: e[1]):
        c_lines.append(f"extern {ret} {fn}({params});")
    c_lines += extern_decls
    c_lines += [""]
    c_lines += defs
    write_if(root / "src/emerald/resources/"
                    "text_skeleton_arrays.generated.c", c_lines, args)


def emit_skeletons(tables, root, args):
    """Per-table fill metadata: every fill's name must be a known C
    resource id (kind 0), a known asm label canonicalized (kind 1), or a
    cut table symbol (kind 2) — fail-closed against hand-edited lists."""
    if len(tables) != PINNED_TEXT_TABLES:
        fail(f"discovered text tables {len(tables)} != pinned "
             f"{PINNED_TEXT_TABLES}")
    for t in tables:
        rows = t.rows
        expected = PINNED_TEXT_TABLE_ROWS.get(t.host_name)
        if expected is None:
            fail(f"table '{t.host_name}' is not in "
                 f"PINNED_TEXT_TABLE_ROWS")
        if (rows, t.kind) != expected:
            fail(f"table '{t.host_name}': (rows {rows}, kind {t.kind}) "
                 f"!= pinned {expected}")
    total_fills = sum(len(t.fills) for t in tables)
    if total_fills != PINNED_SKELETON_FILLS:
        fail(f"skeleton fills {total_fills} != pinned "
             f"{PINNED_SKELETON_FILLS}")
    names = [t.name for t in tables]
    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C §9/§10 skeleton-table fills. Each fill targets one host",
        " * slot by C address expression (no stride/offset math at the",
        " * seam) and names the payload it must receive: kind 0 = per-label",
        " * C resource (full id), kind 1 = bundle-local label (canonical",
        " * name, resolved through the bundle index at its recorded",
        " * blob offset), kind 2 = sub-table pointer row: value is the",
        " * compile-time &Target[row] address (the C compiler owns the",
        " * host row layout; the seam validates and applies it). The",
        " * registry below maps every cut table symbol to its host",
        " * array + row count.",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_SKELETONS_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_SKELETONS_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        "struct TextSkeletonFill",
        "{",
        "    const void **target;  /* C address of the slot, e.g.",
        "                             &gBattleStringsTable[3] or",
        "                             &sMessages[2].text */",
        "    uint32_t kind;        /* 0 = C label (name = full resource id);",
        "                             1 = bundle label (name = canonical,",
        "                             resolved via the bundle index);",
        "                             2 = sub-table (name = target table",
        "                             symbol; fills array[row]) */",
        "    uint32_t row;         /* row index into the target table",
        "                             (kind 2 only) */",
        "    const char *name;     /* resource id / canonical label /",
        "                             table symbol per kind */",
        "    const void *value;    /* kind 2: compile-time &Target[row]",
        "                             (host layout owned by the compiler);",
        "                             NULL for kinds 0/1 */",
        "};",
        "",
        "struct TextSkeletonTableInfo",
        "{",
        "    const char *name;",
        "    const void *array;    /* &NAME[0] of the host array */",
        "    uint32_t rowCount;",
        "    uint32_t fillCount;",
        "    const struct TextSkeletonFill *fills;",
        "};",
        "",
        f"#define TEXT_SKELETON_TABLE_COUNT {len(tables)}u",
        f"#define TEXT_SKELETON_FILL_COUNT {total_fills}u",
        "",
        "extern const struct TextSkeletonTableInfo "
        "kTextSkeletonTables[TEXT_SKELETON_TABLE_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_TEXT_SKELETONS_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/text_skeletons.generated.h",
             lines, args)

    scalar_targets = {t.name for t in tables if t.scalar}
    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/text_skeletons.generated.h"',
        '#include "emerald/resources/text_skeleton_arrays.generated.h"',
        "",
    ]
    for t in tables:
        c_lines.append(f"static const struct TextSkeletonFill "
                       f"kTextSkeletonFills_{t.host_name}[] =")
        c_lines.append("{")
        for target, kind, name, row in t.fills:
            if kind == 2:
                # Scalar target objects have no [row]; the row field
                # stays 0 (the fill points at the object itself).
                if name in scalar_targets:
                    value = f"(const void *)&{name}"
                else:
                    value = f"(const void *)&{name}[{row}]"
            else:
                value = "NULL"
            c_lines.append(f'    {{(const void **){target}, {kind}u, '
                           f'{row}u, "{name}", {value}}},')
        c_lines.append("};")
        c_lines.append("")
    c_lines.append("const struct TextSkeletonTableInfo "
                   "kTextSkeletonTables[TEXT_SKELETON_TABLE_COUNT] =")
    c_lines.append("{")
    for t in tables:
        # Scalar objects (single structs) do not decay; take their
        # address so the base pointer column is valid for every shape.
        base = (f"(const void *)&{t.host_name}" if t.scalar
                else f"(const void *){t.host_name}")
        c_lines.append(f'    {{"{t.host_name}", {base}, '
                       f'{t.rows}u, {len(t.fills)}u, '
                       f'kTextSkeletonFills_{t.host_name}}},')
    c_lines += ["};", ""]
    write_if(root / "src/emerald/resources/text_skeletons_table.generated.c",
             c_lines, args)
    return names, total_fills


def emit_slot_bindings(records, root, args, keymap):
    """Slot -> resource binding for every live C label (needs slot
    re-emission path, R13-C §10): {&gText_X, "emerald:text/..."}."""
    slots = []
    for r in sorted(records, key=lambda r: r[0]):
        if r[3] == "c" and r[1] in LIVE_FAMILIES \
                and r[0] != "gText_123Dot":
            # gText_123Dot: slice table, not a slot (see emit_slots).
            slots.append((r[0], keymap[r[0]]))
    if len(slots) != PINNED_SLOT_BINDINGS:
        fail(f"slot bindings {len(slots)} != pinned "
             f"{PINNED_SLOT_BINDINGS}")
    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C §10 slot bindings: one entry per host-data pointer slot",
        " * (see text_slots.generated.h) naming the published resource the",
        " * seam must fill it from at publish. No hand-maintained list.",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_SLOT_BINDINGS_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_SLOT_BINDINGS_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        "struct TextSlotBinding",
        "{",
        "    const uint8_t *const *slot;  /* &gText_X (the host slot object) */",
        "    const char *name;       /* full resource id */",
        "};",
        "",
        f"#define TEXT_SLOT_BINDING_COUNT {len(slots)}u",
        "",
        "extern const struct TextSlotBinding "
        "kTextSlotBindings[TEXT_SLOT_BINDING_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_TEXT_SLOT_BINDINGS_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/"
                    "text_slot_bindings.generated.h", lines, args)

    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/text_slot_bindings.generated.h"',
        '#include "emerald/resources/text_slots.generated.h"',
        "",
        "const struct TextSlotBinding "
        "kTextSlotBindings[TEXT_SLOT_BINDING_COUNT] =",
        "{",
    ]
    for label, rid in slots:
        c_lines.append(f'    {{&{label}, "{rid}"}},')
    c_lines += ["};", ""]
    write_if(root / "src/emerald/resources/"
                    "text_slot_bindings.generated.c", c_lines, args)


def emit_bundle_index(records, bundle_blobs, root, args):
    """Bundle-local label index (R13-C §12): every asm label's
    (bundleIndex, blobOffset, size, arenaIndex), sorted by canonical
    name, plus the sorted bundle id table. Layout mirrors
    bundle_blob(): count u32, dataOffset u32, offsets, payload."""
    bundles = sorted(bundle_blobs)
    ids = []
    entries = []
    for b, rid in enumerate(bundles):
        ids.append(rid)
        recs = sorted((r for r in records if r[3] == "asm"
                       and bundle_key("asm", r[2]) == rid),
                      key=lambda r: canonical(r[0]))
        data_off = 8 + 4 * len(recs)
        off = data_off
        for r in recs:
            entries.append((canonical(r[0]), b, off, r[6],
                            ARENA_KEYS.index(arena_of(r))))
            off += r[6]
    entries.sort(key=lambda e: e[0])
    if len(entries) != PINNED_BUNDLE_INDEX_ENTRIES:
        fail(f"bundle index entries {len(entries)} != pinned "
             f"{PINNED_BUNDLE_INDEX_ENTRIES}")
    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C §12 bundle-local label index: stable label-level offsets",
        " * inside every script bundle blob (future ROM-hack importing can",
        " * emit targeted overrides); also the sorted bundle id table the",
        " * seam resolves kind-1 fills through.",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_BUNDLE_INDEX_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_BUNDLE_INDEX_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        "struct TextBundleEntry",
        "{",
        "    const char *name;       /* canonical label (lower-dash) */",
        "    uint32_t bundleIndex;   /* index into kTextBundleIds */",
        "    uint32_t blobOffset;    /* byte offset in the bundle blob */",
        "    uint32_t size;          /* canonical bytes (incl. 0xFF) */",
        "    uint32_t arenaIndex;    /* arena (ARENA_KEYS order) */",
        "};",
        "",
        f"#define TEXT_BUNDLE_ENTRY_COUNT {len(entries)}u",
        f"#define TEXT_BUNDLE_COUNT {len(ids)}u",
        "",
        "extern const struct TextBundleEntry "
        "kTextBundleIndex[TEXT_BUNDLE_ENTRY_COUNT];",
        "extern const char *const kTextBundleIds[TEXT_BUNDLE_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_TEXT_BUNDLE_INDEX_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/"
                    "text_bundle_index.generated.h", lines, args)

    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/text_bundle_index.generated.h"',
        "",
        "const struct TextBundleEntry "
        "kTextBundleIndex[TEXT_BUNDLE_ENTRY_COUNT] =",
        "{",
    ]
    for name, b, off, size, arena in entries:
        c_lines.append(f'    {{"{name}", {b}u, {off}u, {size}u, {arena}u}},')
    c_lines += ["};", "",
                "const char *const kTextBundleIds[TEXT_BUNDLE_COUNT] =",
                "{"]
    for rid in ids:
        c_lines.append(f'    "{rid}",')
    c_lines += ["};", ""]
    write_if(root / "src/emerald/resources/"
                    "text_bundle_index.generated.c", c_lines, args)


def emit_arenas(records, root, args):
    """Per-family arena summaries (R13-C §8/§14): 16 arenas, keyed in
    ARENA_KEYS order, each with its pinned label count + canonical byte
    total. The seam re-derives these from the resolution table at
    publish and fails on any mismatch."""
    per = {}
    for r in records:
        per.setdefault(arena_of(r), [0, 0])
        per[arena_of(r)][0] += 1
        per[arena_of(r)][1] += r[6]
    for key in ARENA_KEYS:
        if key not in per:
            fail(f"arena '{key}' has no labels")
    bad = [k for k in ARENA_KEYS
           if tuple(per[k]) != PINNED_ARENA_SUMMARIES[k]]
    if bad:
        fail("arena summaries mismatch for " +
             ", ".join(f"{k} measured {per[k]} != pinned "
                       f"{PINNED_ARENA_SUMMARIES[k]}" for k in bad))
    lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " *",
        " * R13-C §8/§14 per-family deterministic arenas: 16 summary rows.",
        " * labelCount is the number of labels (C + bundle-local) packed",
        " * into the arena; byteTotal is their canonical bytes (incl.",
        " * 0xFF terminators) before 16-alignment padding.",
        " */",
        "#ifndef EMERALD_RESOURCES_TEXT_ARENAS_GENERATED_H",
        "#define EMERALD_RESOURCES_TEXT_ARENAS_GENERATED_H",
        "",
        "#include <stdint.h>",
        "",
        "struct TextArenaSummary",
        "{",
        "    const char *key;",
        "    uint32_t labelCount;",
        "    uint32_t byteTotal;",
        "};",
        "",
        f"#define TEXT_ARENA_COUNT {len(ARENA_KEYS)}u",
        "",
        "extern const struct TextArenaSummary "
        "kTextArenaSummaries[TEXT_ARENA_COUNT];",
        "",
        "#endif /* EMERALD_RESOURCES_TEXT_ARENAS_GENERATED_H */",
    ]
    write_if(root / "include/emerald/resources/text_arenas.generated.h",
             lines, args)

    c_lines = [
        "/* Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        " * Do not edit by hand; re-run the generator and --check it.",
        " */",
        '#include "emerald/resources/text_arenas.generated.h"',
        "",
        "const struct TextArenaSummary "
        "kTextArenaSummaries[TEXT_ARENA_COUNT] =",
        "{",
    ]
    for key in ARENA_KEYS:
        count, total = per[key]
        c_lines.append(f'    {{"{key}", {count}u, {total}u}},')
    c_lines += ["};", ""]
    write_if(root / "src/emerald/resources/text_arenas.generated.c",
             c_lines, args)


def emit_skeleton_report(tables, deferred, records, fam_dir, args):
    """Surface report: every discovered table (file/kind/rows/fills), the
    mixed live/deferred tables that stay compiled, and every arena
    summary, for the R13-C report + review."""
    per = {}
    for r in records:
        per.setdefault(arena_of(r), [0, 0])
        per[arena_of(r)][0] += 1
        per[arena_of(r)][1] += r[6]
    lines = [
        "# Generated by tools/gen3_resources/text_family/gen_text_family.py.",
        "# R13-C §9/§10 skeleton-table surface report (do not edit).",
        "",
    ]
    for t in tables:
        kind = "struct" if t.sname and not t.star \
            else "ptr-table" if t.star else ("ptr2" if len(t.shape) > 1
                                             else "ptr")
        lines += [f"[[tables]]",
                  f'file = "{t.rel}"',
                  f'name = "{t.host_name}"',
                  f'struct = "{t.sname}"',
                  f'kind = "{kind}"',
                  f'rows = {t.rows}',
                  f'fills = {len(t.fills)}',
                  ""]
    lines.append("")
    for key in ARENA_KEYS:
        count, total = per[key]
        lines += [f"[[arenas]]",
                  f'key = "{key}"',
                  f'label_count = {count}',
                  f'byte_total = {total}',
                  ""]
    if deferred:
        lines.append("")
        for entry in deferred:
            if len(entry) == 4:
                rel, name, fams, reason = entry
            else:
                rel, name, fams = entry
                reason = "mixed live/deferred families"
            lines += [f"[[deferred_tables]]",
                      f'file = "{rel}"',
                      f'name = "{name}"',
                      f'families = "{",".join(fams)}"',
                      f'reason = "{reason}"',
                      ""]
    write_if(fam_dir / "skeleton_tables.generated.toml", lines, args)


# R13-C §9/§10 measured pins (fail-closed). Filled from the measured
# discovery; every emission re-asserts them.
PINNED_TEXT_TABLES = 283
PINNED_TEXT_TABLE_ROWS = {
    "MultichoiceList_AcroBikeInfo": (4, "text"),
    "MultichoiceList_BasePCNoRegistry": (3, "text"),
    "MultichoiceList_BasePCWithRegistry": (4, "text"),
    "MultichoiceList_BattleArenaRules": (5, "text"),
    "MultichoiceList_BattleDomeRules": (4, "text"),
    "MultichoiceList_BattleFactoryRules": (6, "text"),
    "MultichoiceList_BattleMode": (5, "text"),
    "MultichoiceList_BattlePalaceRules": (6, "text"),
    "MultichoiceList_BattlePikeRules": (4, "text"),
    "MultichoiceList_BattlePyramidRules": (5, "text"),
    "MultichoiceList_BattleTowerFeelings": (4, "text"),
    "MultichoiceList_BattleTowerRules": (5, "text"),
    "MultichoiceList_Bike": (2, "text"),
    "MultichoiceList_BrineyOffDewford": (2, "text"),
    "MultichoiceList_BrineyOnDewford": (3, "text"),
    "MultichoiceList_ChallengeInfo": (3, "text"),
    "MultichoiceList_ContestInfo": (4, "text"),
    "MultichoiceList_ContestRank": (5, "text"),
    "MultichoiceList_ContestType": (6, "text"),
    "MultichoiceList_EnterInfo": (3, "text"),
    "MultichoiceList_Exit": (1, "text"),
    "MultichoiceList_FallarborTentRules": (6, "text"),
    "MultichoiceList_Floors": (6, "text"),
    "MultichoiceList_ForcedStartMenu": (8, "text"),
    "MultichoiceList_Fossil": (3, "text"),
    "MultichoiceList_FrontierGamblerBet": (4, "text"),
    "MultichoiceList_FrontierItemChoose": (3, "text"),
    "MultichoiceList_FrontierPassInfo": (4, "text"),
    "MultichoiceList_FrontierRules": (6, "text"),
    "MultichoiceList_GameCornerCoins": (3, "text"),
    "MultichoiceList_GameCornerDolls": (4, "text"),
    "MultichoiceList_GameCornerTMs": (6, "text"),
    "MultichoiceList_GoOnRecordRestRetire": (4, "text"),
    "MultichoiceList_GoOnRecordRetire": (3, "text"),
    "MultichoiceList_GoOnRestRetire": (3, "text"),
    "MultichoiceList_GoOnRetire": (2, "text"),
    "MultichoiceList_HowsFishing": (2, "text"),
    "MultichoiceList_LevelMode": (3, "text"),
    "MultichoiceList_LinkContestInfo": (4, "text"),
    "MultichoiceList_LinkContestMode": (3, "text"),
    "MultichoiceList_LinkLeader": (3, "text"),
    "MultichoiceList_LinkServicesAll": (5, "text"),
    "MultichoiceList_LinkServicesNoBerry": (4, "text"),
    "MultichoiceList_LinkServicesNoRecord": (4, "text"),
    "MultichoiceList_LinkServicesNoRecordBerry": (3, "text"),
    "MultichoiceList_MachBikeInfo": (4, "text"),
    "MultichoiceList_Mechadoll1_Q1": (3, "text"),
    "MultichoiceList_Mechadoll1_Q2": (3, "text"),
    "MultichoiceList_Mechadoll1_Q3": (3, "text"),
    "MultichoiceList_Mechadoll2_Q1": (3, "text"),
    "MultichoiceList_Mechadoll2_Q2": (3, "text"),
    "MultichoiceList_Mechadoll2_Q3": (3, "text"),
    "MultichoiceList_Mechadoll3_Q1": (3, "text"),
    "MultichoiceList_Mechadoll3_Q2": (3, "text"),
    "MultichoiceList_Mechadoll3_Q3": (3, "text"),
    "MultichoiceList_Mechadoll4_Q1": (3, "text"),
    "MultichoiceList_Mechadoll4_Q2": (3, "text"),
    "MultichoiceList_Mechadoll4_Q3": (3, "text"),
    "MultichoiceList_Mechadoll5_Q1": (3, "text"),
    "MultichoiceList_Mechadoll5_Q2": (3, "text"),
    "MultichoiceList_Mechadoll5_Q3": (3, "text"),
    "MultichoiceList_RegisterMenu": (4, "text"),
    "MultichoiceList_RightLeft": (2, "text"),
    "MultichoiceList_SSTidalBattleFrontier": (3, "text"),
    "MultichoiceList_SSTidalSlateportNoBF": (2, "text"),
    "MultichoiceList_SSTidalSlateportWithBF": (3, "text"),
    "MultichoiceList_Satisfaction": (2, "text"),
    "MultichoiceList_ShardsB": (2, "text"),
    "MultichoiceList_ShardsBG": (3, "text"),
    "MultichoiceList_ShardsG": (2, "text"),
    "MultichoiceList_ShardsR": (2, "text"),
    "MultichoiceList_ShardsRB": (3, "text"),
    "MultichoiceList_ShardsRBG": (4, "text"),
    "MultichoiceList_ShardsRG": (3, "text"),
    "MultichoiceList_ShardsRY": (3, "text"),
    "MultichoiceList_ShardsRYB": (4, "text"),
    "MultichoiceList_ShardsRYBG": (5, "text"),
    "MultichoiceList_ShardsRYG": (4, "text"),
    "MultichoiceList_ShardsY": (2, "text"),
    "MultichoiceList_ShardsYB": (3, "text"),
    "MultichoiceList_ShardsYBG": (4, "text"),
    "MultichoiceList_ShardsYG": (3, "text"),
    "MultichoiceList_SlateportTentRules": (6, "text"),
    "MultichoiceList_StatusInfo": (6, "text"),
    "MultichoiceList_SternDeepSea": (3, "text"),
    "MultichoiceList_TVLati": (2, "text"),
    "MultichoiceList_TagMatchType": (5, "text"),
    "MultichoiceList_Tent": (2, "text"),
    "MultichoiceList_TourneyNoRecord": (5, "text"),
    "MultichoiceList_TourneyWithRecord": (6, "text"),
    "MultichoiceList_UnusedAshVendor": (8, "text"),
    "MultichoiceList_UnusedSSTidal1": (3, "text"),
    "MultichoiceList_UnusedSSTidal2": (3, "text"),
    "MultichoiceList_UnusedSSTidal3": (3, "text"),
    "MultichoiceList_UnusedSSTidal4": (4, "text"),
    "MultichoiceList_VendingMachine": (4, "text"),
    "MultichoiceList_ViewedPaintings": (2, "text"),
    "MultichoiceList_WheresRayquaza": (4, "text"),
    "MultichoiceList_WirelessMinigame": (3, "text"),
    "MultichoiceList_YesNo": (2, "text"),
    "MultichoiceList_YesNoInfo": (3, "text"),
    "MultichoiceList_YesNoInfo2": (3, "text"),
    "gAbilityDescriptionPointers": (78, "text"),
    "gBagMenu_ReturnToStrings": (12, "text"),
    "gBattleStringsTable": (369, "text"),
    "gContestEffectDescriptionPointers": (48, "text"),
    "gContestMoveTypeTextPointers": (5, "text"),
    "gGiftRibbonDescriptionPointers": (64, "text"),
    "gMailboxMailOptions": (4, "text"),
    "gMatchCallFlavorTexts": (78, "text"),
    "gMoveDescriptionPointers": (354, "text"),
    "gNatureNamePointers": (25, "text"),
    "gPocketNamesStringsTable": (5, "text"),
    "gPokeblockNames": (15, "text"),
    "gPokeblockWasTooXStringTable": (5, "text"),
    "gPyramidBagMenu_ReturnToStrings": (4, "text"),
    "gRefereeStringsTable": (9, "text"),
    "gRibbonDescriptionPointers": (25, "text"),
    "gRoundsStringTable": (4, "text"),
    "gStatNamesTable": (8, "text"),
    "gStatusConditionStringsTable": (7, "text"),
    "gStdStrings": (30, "text"),
    "gTextTable_Players": (4, "text"),
    "gText_123Dot": (3, "text"),
    "sActionStringTable": (27, "text"),
    "sAppealResultTexts": (62, "text"),
    "sBattleFrontierFacilityNames": (7, "text"),
    "sBattleFrontier_TutorMoveDescriptions1": (11, "text"),
    "sBattleFrontier_TutorMoveDescriptions2": (11, "text"),
    "sBerryFirmnessStrings": (5, "text"),
    "sBrawlyMatchCallHeader": (1, "text"),
    "sBrawlyTextScripts": (5, "text"),
    "sBrendanMatchCallHeader": (1, "text"),
    "sBrendanTextScripts": (16, "text"),
    "sCheckPageOverrides": (4, "text"),
    "sCompatibilityMessages": (4, "text"),
    "sConditionNames": (5, "text"),
    "sContestCategoryNames_Unused": (5, "text"),
    "sContestConditions": (5, "text"),
    "sContestLadyCategoryNames": (5, "text"),
    "sContestLadyMonNames": (5, "text"),
    "sContestNames": (5, "text"),
    "sContestRankNames": (5, "text"),
    "sDecorationCategoryNames": (8, "text"),
    "sDecorationMainMenuActions": (4, "text"),
    "sDefaultTraderNames": (4, "text"),
    "sDeptStoreFloorNames": (16, "text"),
    "sDescriptionStringTable": (13, "text"),
    "sDexModeOptions": (3, "text"),
    "sDexOrderOptions": (7, "text"),
    "sDexSearchColorOptions": (12, "text"),
    "sDexSearchNameOptions": (11, "text"),
    "sDexSearchTypeOptions": (19, "text"),
    "sDisplayStdMessages": (11, "text"),
    "sDrakeMatchCallHeader": (1, "text"),
    "sDrakeTextScripts": (2, "text"),
    "sEasyChatGroupNamePointers": (22, "text"),
    "sEasyChatScreenTemplates": (21, "text"),
    "sEverGrandeCityNames": (2, "text"),
    "sFavorLadyRequests": (6, "text"),
    "sFemalePresetNames": (20, "text"),
    "sFlanneryMatchCallHeader": (1, "text"),
    "sFlanneryTextScripts": (5, "text"),
    "sFloorStrings": (4, "text"),
    "sFooterTextOptions": (3, "text"),
    "sFrontierExchangeCorner_Decor1Descriptions": (11, "text"),
    "sFrontierExchangeCorner_Decor2Descriptions": (6, "text"),
    "sFrontierExchangeCorner_HoldItemsDescriptions": (10, "text"),
    "sFrontierExchangeCorner_VitaminsDescriptions": (7, "text"),
    "sGlaciaMatchCallHeader": (1, "text"),
    "sGlaciaTextScripts": (2, "text"),
    "sHallFacilityToRecordsText": (10, "text"),
    "sHeaderTexts": (5, "text"),
    "sHelpBarTexts": (12, "text"),
    "sInvalidContestMoveNames": (6, "text"),
    "sItemMenuActions": (15, "text"),
    "sItemStorage_MenuActions": (4, "text"),
    "sItemStorage_OptionDescriptions": (4, "text"),
    "sJuanMatchCallHeader": (1, "text"),
    "sJuanTextScripts": (5, "text"),
    "sKeyboardPageTitleTexts": (5, "text"),
    "sLevelMenuItems": (3, "text"),
    "sLevelModeText": (2, "text"),
    "sLilycoveSSTidalDestinations": (7, "text"),
    "sLinkBattleTexts": (3, "text"),
    "sListMenuItems_CardsOrNews": (3, "text"),
    "sListMenuItems_Receive": (2, "text"),
    "sListMenuItems_ReceiveSend": (3, "text"),
    "sListMenuItems_ReceiveSendToss": (4, "text"),
    "sListMenuItems_ReceiveToss": (3, "text"),
    "sListMenuItems_WirelessOrFriend": (3, "text"),
    "sLvlUpStatStrings": (6, "text"),
    "sMalePresetNames": (20, "text"),
    "sMatchCallBattleDomeTexts": (14, "text"),
    "sMatchCallBattleFrontierRecordStreakTexts": (14, "text"),
    "sMatchCallBattleFrontierStreakTexts": (14, "text"),
    "sMatchCallBattlePikeTexts": (14, "text"),
    "sMatchCallBattlePyramidTexts": (14, "text"),
    "sMatchCallBattleRequestTopics": (2, "subtable"),
    "sMatchCallBattleTopics": (3, "subtable"),
    "sMatchCallDifferentRouteBattleRequestTexts": (14, "text"),
    "sMatchCallGeneralTopics": (6, "subtable"),
    "sMatchCallNegativeBattleTexts": (14, "text"),
    "sMatchCallOptionTexts": (3, "text"),
    "sMatchCallPersonalizedTexts": (64, "text"),
    "sMatchCallPositiveBattleTexts": (14, "text"),
    "sMatchCallSameRouteBattleRequestTexts": (14, "text"),
    "sMatchCallWildBattleTexts": (15, "text"),
    "sMayMatchCallHeader": (1, "text"),
    "sMayTextScripts": (16, "text"),
    "sMenuActions": (6, "text"),
    "sMenuActions_Gender": (2, "text"),
    "sMenuTexts": (39, "text"),
    "sMessages": (9, "text"),
    "sMessages_berry_crush": (9, "text"),
    "sMessages_pokemon_storage_system": (31, "text"),
    "sModeStrings": (4, "text"),
    "sMomMatchCallHeader": (1, "text"),
    "sMomTextScripts": (4, "text"),
    "sMonNamingScreenTemplate": (1, "text"),
    "sMrStoneMatchCallHeader": (1, "text"),
    "sMrStoneTextScripts": (12, "text"),
    "sMultiTrainerMatchCallTexts": (6, "text"),
    "sMuseumCaptions": (15, "text"),
    "sNamingScreenTemplates": (5, "subtable"),
    "sNormanMatchCallHeader": (1, "text"),
    "sNormanTextScripts": (10, "text"),
    "sOptionMenuItemsNames": (8, "text"),
    "sPCBoxNamingTemplate": (1, "text"),
    "sPCNameStrings": (4, "text"),
    "sPageDescriptions": (14, "text"),
    "sPassAreaDescriptions": (15, "text"),
    "sPhoebeMatchCallHeader": (1, "text"),
    "sPhoebeTextScripts": (2, "text"),
    "sPlayerNamingScreenTemplate": (1, "text"),
    "sPlayerPCMenuActions": (4, "text"),
    "sPokeblockMenuActions": (6, "text"),
    "sProfBirchMatchCallHeader": (1, "text"),
    "sPyramidFloorNames": (8, "text"),
    "sRankingTexts": (5, "text"),
    "sRecordsTexts_dodrio_berry_picking": (3, "text"),
    "sRecordsTexts_pokemon_jump": (3, "text"),
    "sRecordsWindowChallengeTexts": (10, "text"),
    "sRegisterForTradeListMenuItems": (3, "text"),
    "sRegistryMenuActions": (2, "text"),
    "sResultsTexts": (6, "text"),
    "sRoundResultTexts": (32, "text"),
    "sRoxanneMatchCallHeader": (1, "text"),
    "sRoxanneTextScripts": (5, "text"),
    "sScottMatchCallHeader": (1, "text"),
    "sScottTextScripts": (8, "text"),
    "sScrollableMultichoiceOptions": (13, "text"),
    "sSearchMenuItems": (7, "text"),
    "sSearchMenuTopBarItems": (3, "text"),
    "sSecretBasePCMenuItemDescriptions": (4, "text"),
    "sShopMenuActions_BuyQuit": (2, "text"),
    "sShopMenuActions_BuySellQuit": (3, "text"),
    "sSidneyMatchCallHeader": (1, "text"),
    "sSidneyTextScripts": (2, "text"),
    "sStartMenuItems": (13, "text"),
    "sStatNamesTable2": (6, "text"),
    "sStevenMatchCallHeader": (1, "text"),
    "sStevenTextScripts": (8, "text"),
    "sTateLizaMatchCallHeader": (1, "text"),
    "sTateLizaTextScripts": (5, "text"),
    "sTrainerCardColorNames": (4, "text"),
    "sTransferredToPCMessages": (4, "text"),
    "sUnionRoomTradeMessages": (9, "text"),
    "sUnusedAppealResultTexts": (13, "text"),
    "sUnusedComboMoveNameTexts": (14, "text"),
    "sUnusedMenuTexts": (4, "text"),
    "sUnused_StatStrings": (6, "text"),
    "sWaldaWordsScreenTemplate": (1, "text"),
    "sWallaceMatchCallHeader": (1, "text"),
    "sWallaceTextScripts": (2, "text"),
    "sWallyMatchCallHeader": (1, "text"),
    "sWallyTextScripts": (8, "text"),
    "sWattsonMatchCallHeader": (1, "text"),
    "sWattsonTextScripts": (5, "text"),
    "sWinonaMatchCallHeader": (1, "text"),
    "sWinonaTextScripts": (5, "text"),
    # R13-C #110: named structs (moved to pokemon_storage_system.h /
    # frontier_pass.h) made discovery pick up these two tables.
    "sMainMenuTexts": (5, "text"),
    "sMapLandmarks": (7, "text"),
}
PINNED_SKELETON_FILLS = 3335
PINNED_ARENA_SUMMARIES = {
    "battle": (522, 11989),
    "move": (355, 17251),
    "ability": (78, 1859),
    "nature": (25, 162),
    "item": (310, 15101),
    "pokedex": (387, 58017),
    "system-shared": (2268, 58705),
    "tv": (401, 55507),
    "matchcall": (629, 60934),
    "apprentice": (288, 49891),
    "ribbon": (66, 1265),
    "frontier": (180, 11597),
    "frontier-brain": (28, 882),
    "easy-chat": (1008, 7101),
    "berry": (41, 4305),
    "misc": (6191, 548591),
}

PINNED_BUNDLE_INDEX_ENTRIES = 7953
# Slots cover all eight live families: battle 522 + move 355 + ability
# 78 + nature 25 + shared 1655 + system 106 + match-call 312 + ribbon 66.
# A slot for a label reachable only via a cut table is harmless host
# metadata; a missing slot would break the link, so the superset is the
# fail-safe direction.
# gText_123Dot is excluded: it is a 3-row slice table (frontier_util.c
# indexes gText_123Dot[position], which a pointer slot would break), not
# a slot; emit_slots/emit_slot_bindings skip it and the synthetic table
# carries the slice fills.
PINNED_SLOT_BINDINGS = 3118



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("elf", nargs="?",
                    default="../pokeemerald-reference/pokeemerald.elf")
    ap.add_argument("rom", nargs="?",
                    default="../pokeemerald-reference/pokeemerald.gba")
    ap.add_argument("outdir", nargs="?",
                    default="resources/extraction/emerald/bpee01")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    global root, rom
    root = Path(args.root)
    fam_dir = Path(args.outdir) / "text"
    rom = open(args.rom, "rb").read()
    if len(rom) != ROM_SIZE:
        fail(f"ROM size {len(rom)} != 16 MiB")
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        fail(f"ROM SHA-1 != {ROM_SHA1} (retail-matching ROM required)")

    elf = Elf32(open(args.elf, "rb").read())
    decode = make_decode(Path(args.elf).parent / "charmap.txt")
    if not decode(b"ABC\xff"):
        fail("charmap validator self-check failed")
    if not decode(b"A\xfa\xff"):
        fail("charmap validator self-check failed (FA single)")

    records = resolve(root, elf, rom, decode, args)
    check_pins(records)

    c_records = [r for r in records if r[3] == "c"]
    keymap = assign_label_keys(c_records)
    if len(keymap) != PINNED_C_RESOURCES or \
            set(keymap) != {r[0] for r in c_records}:
        fail(f"keymap {len(keymap)} != {PINNED_C_RESOURCES} "
             f"(set mismatch)")

    asm_labels = sorted(r[0] for r in records if r[3] == "asm")
    dual = find_dual_refs(root, asm_labels)
    if len(dual) != PINNED_DUAL_REF:
        fail(f"dual-ref labels {len(dual)} != pinned {PINNED_DUAL_REF}")

    label_arts, bundle_blobs = emit_artifacts(records, fam_dir, args, rom,
                                              keymap)
    emit_family(records, dual, label_arts, bundle_blobs, fam_dir, args,
                keymap)
    emit_seam_header(records, label_arts, bundle_blobs, root, args, keymap)
    emit_slots(records, root, args)

    tables, deferred = discover_text_tables(root, elf, records)
    deferred += analyze_text_tables(tables, records, keymap, elf, root)
    emit_arenas(records, root, args)
    emit_slot_bindings(records, root, args, keymap)
    emit_bundle_index(records, bundle_blobs, root, args)
    emit_skeleton_arrays(tables, root, args)
    emit_skeletons(tables, root, args)
    emit_skeleton_report(tables, deferred, records, fam_dir, args)

    print(f"text labels: {len(records)} "
          f"({PINNED_C_RESOURCES} C-side per-label + "
          f"{PINNED_BUNDLES} bundles covering {PINNED_ASM_LABELS} asm "
          f"labels)")
    print(f"canonical bytes: {sum(r[6] for r in records)} B; bundles "
          f"{sum(len(b) for b in bundle_blobs.values())} B of blobs")
    print(f"dual-ref labels: {len(dual)}; text tables: {len(tables)}; "
          f"fills: {sum(len(t.fills) for t in tables)}")
    if deferred:
        print(f"deferred tables: {len(deferred)} "
              f"({', '.join(e[1] for e in deferred)})")


if __name__ == "__main__":
    main()

