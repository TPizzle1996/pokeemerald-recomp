#!/usr/bin/env python3
"""R11-C: machine-readable tileset graphics inventory.

Parses the checked-in sources of truth and emits
resources/extraction/emerald/bpee01/tileset/inventory.generated.toml:

  * src/data/tilesets/headers.h
      -> the 75 `struct Tileset` objects: isCompressed / isSecondary /
         tiles / palettes / metatiles / metatileAttributes / callback
  * src/data/tilesets/graphics.h + src/graphics.c
      -> gTilesetTiles_* tile leaves (INCBIN_U32; .lz = compressed,
         bare .4bpp = raw) and gTilesetPalettes_*[][16] palette arrays
         (16 INCBIN_U16 rows each) - General lives in graphics.c,
         UnionRoom proves tiles can follow palettes
      -> gTilesetAnims_BattleDomePals0_0..3 floor-light palette leaves
         (graphics.c) - the one raw-u16 palette resource of the family
  * src/data/tilesets/metatiles.h
      -> 70 gMetatiles_* + 70 gMetatileAttributes_* INCBIN_U16 blobs;
         the SecretBase color variants share the SecretBaseSecondary pair
         (aliasing: one resource, six consumers)
  * src/tileset_anims.c
      -> 135 gTilesetAnims_<Anim>_Frame<n> u16 leaves (INCBIN_U16, one or
         more artifacts - Sootopolis StormyWater concatenates kyogre +
         groudon variants, the preproc emits every argument) and the
         compiled `gTilesetAnims_<Anim>[]` frame tables that reference
         them; tileset_anims_space_* spacers are skipped

Deterministic: all rows bytewise sorted by symbol/canonical. Regeneration
must be a no-op diff; --check enforces that.

Pins (fail hard on deviation): 75 structs (3 primary / 72 secondary),
75 referenced tile leaves + 8 GBA-parity unreferenced leaves, 75 palette
arrays = 1,200 rows, 70 metatile + 70 attribute blobs, 135 anim-frame
leaves (125 consumed by frame tables, 10 GBA-parity unreferenced:
Lavaridge_Cave_Lava Frame4-7 + Unused1/Unused2), 4 floor-light palette
leaves, 31 frame tables. Duplicate canonical ids,
invalid struct symbols, missing consumers, inconsistent aliasing and
compression-flag mismatches are hard failures.

Usage: python3 gen_tileset_inventory.py [repo-root] [output-path] [--check]
"""
import re
import sys
from collections import OrderedDict

STRUCT_RE = re.compile(r'const struct Tileset (gTileset_\w+) =')
STRUCT_FIELD_RE = re.compile(r'\.(\w+) = (\w+),?')
TILE_RE = re.compile(r'const u32 (gTilesetTiles_\w+)\[\] = INCBIN_U32\(([^)]*)\);')
PAL_ARRAY_RE = re.compile(r'const u16 (gTilesetPalettes_\w+)\[\]\[16\] =')
PAL_ROW_RE = re.compile(r'INCBIN_U16\("([^"]+)"\)')
FLOOR_LIGHT_RE = re.compile(
    r'const u16 (gTilesetAnims_BattleDomePals0_\d+)\[\] = INCBIN_U16\("([^"]+)"\);')
METATILES_RE = re.compile(r'const u16 (gMetatiles_\w+)\[\] = INCBIN_U16\("([^"]+)"\);')
ATTRIBUTES_RE = re.compile(
    r'const u16 (gMetatileAttributes_\w+)\[\] = INCBIN_U16\("([^"]+)"\);')
ANIM_FRAME_RE = re.compile(
    r'const u16 (gTilesetAnims_\w+)_Frame(\d+)\[\] = INCBIN_U16\(([^)]*)\);')
ANIM_TABLE_RE = re.compile(r'const u16 \*const (gTilesetAnims_\w+)\[\] = \{')
FLOOR_LIGHT_TABLE_RE = re.compile(
    r'static const u16 \*const (sTilesetAnims_BattleDomeFloorLightPals)\[\] = \{')
FRAME_REF_RE = re.compile(r'(gTilesetAnims_\w+_Frame\d+),?')

HEADERS_H = "src/data/tilesets/headers.h"
GRAPHICS_H = "src/data/tilesets/graphics.h"
GRAPHICS_C = "src/graphics.c"
METATILES_H = "src/data/tilesets/metatiles.h"
TILESET_ANIMS_C = "src/tileset_anims.c"

PIN_STRUCTS = 75
PIN_PRIMARY = 3
PIN_PALETTE_ARRAYS = 75
PIN_PALETTE_ROWS = 1200
PIN_METATILES = 70
PIN_ATTRIBUTES = 70
PIN_ANIM_FRAMES = 135
PIN_ANIM_PARITY = 10
PIN_PARITY_TILES = 8
PIN_FRAME_TABLES = 31

# The 8 unreferenced tile leaves (documented GBA_PARITY, no canonical id).
PARITY_TILES = frozenset([
    "gTilesetTiles_SecretBaseBrownCaveCompressed",
    "gTilesetTiles_SecretBaseTreeCompressed",
    "gTilesetTiles_SecretBaseShrubCompressed",
    "gTilesetTiles_SecretBaseBlueCaveCompressed",
    "gTilesetTiles_SecretBaseYellowCaveCompressed",
    "gTilesetTiles_SecretBaseRedCaveCompressed",
    "gTilesetTiles_UnknownCableClub",
    "gTilesetTiles_UnknownSecretBase",
])


def canonicalize(symbol):
    """R11-B canonical rule: lowercase symbol suffix, '_' -> '-'.
    e.g. gTilesetTiles_Petalburg -> petalburg,
         gTilesetAnims_General_Flower_Frame1 -> general-flower (frame 1)."""
    return symbol.lower().replace("_", "-")


def canonical_suffix(symbol, prefix):
    """Lowercased, '_'->'-' suffix after a known leaf prefix."""
    if not symbol.startswith(prefix):
        sys.exit(f"FAIL: {symbol} does not start with {prefix}")
    return canonicalize(symbol[len(prefix):])


def split_incbin_args(arglist):
    """Split the comma-separated INCBIN argument list into file paths
    (the preproc concatenates every argument; keep them in order)."""
    args = []
    for raw in arglist.split(","):
        m = re.match(r'\s*"([^"]+)"\s*', raw)
        if m is None:
            sys.exit(f"FAIL: malformed INCBIN argument: {raw.strip()}")
        args.append(m.group(1))
    if not args:
        sys.exit("FAIL: empty INCBIN argument list")
    return args


def parse_structs(path):
    structs = OrderedDict()
    for i, line in enumerate(open(path)):
        m = STRUCT_RE.match(line.strip())
        if not m:
            continue
        name = m.group(1)
        if name in structs:
            sys.exit(f"FAIL: duplicate tileset struct {name} (line {i + 1})")
        fields = {}
        block = []
        for j, sl in enumerate(open(path)):
            if j <= i:
                continue
            block.append(sl.strip())
            if sl.strip() == "};":
                break
        for bl in block:
            fm = STRUCT_FIELD_RE.match(bl)
            if fm:
                fields[fm.group(1)] = fm.group(2)
        for required in ("isCompressed", "isSecondary", "tiles", "palettes",
                         "metatiles", "metatileAttributes", "callback"):
            if required not in fields:
                sys.exit(f"FAIL: {name} (line {i + 1}) missing .{required}")
        if fields["isCompressed"] not in ("TRUE", "FALSE"):
            sys.exit(f"FAIL: {name} (line {i + 1}) bad .isCompressed")
        if fields["isSecondary"] not in ("TRUE", "FALSE"):
            sys.exit(f"FAIL: {name} (line {i + 1}) bad .isSecondary")
        structs[name] = fields
    return structs


def parse_tile_leaves(path):
    leaves = OrderedDict()
    for i, line in enumerate(open(path)):
        m = TILE_RE.match(line.strip())
        if m:
            if m.group(1) in leaves:
                sys.exit(f"FAIL: duplicate tile leaf {m.group(1)} (line {i + 1})")
            leaves[m.group(1)] = split_incbin_args(m.group(2))
    return leaves


def parse_palette_arrays(path):
    arrays = OrderedDict()
    current = None
    rows = []
    for i, line in enumerate(open(path)):
        stripped = line.strip()
        m = PAL_ARRAY_RE.match(stripped)
        if m:
            if current is not None:
                sys.exit(f"FAIL: palette array {current} (line {i + 1}) "
                         "not terminated with };")
            name = m.group(1)
            if name in arrays:
                sys.exit(f"FAIL: duplicate palette array {name} (line {i + 1})")
            current = name
            rows = []
            continue
        if current is not None:
            rm = PAL_ROW_RE.match(stripped)
            if rm:
                rows.append(rm.group(1))
                continue
            if stripped == "};":
                if len(rows) != 16:
                    sys.exit(f"FAIL: palette array {current} has {len(rows)} "
                             "rows (expected 16)")
                arrays[current] = rows
                current = None
                rows = []
                continue
    if current is not None:
        sys.exit(f"FAIL: palette array {current} not terminated with '}}';")
    return arrays


def parse_floor_light_pals(path):
    pals = OrderedDict()
    for i, line in enumerate(open(path)):
        m = FLOOR_LIGHT_RE.match(line.strip())
        if m:
            if m.group(1) in pals:
                sys.exit(f"FAIL: duplicate floor-light palette {m.group(1)} "
                         f"(line {i + 1})")
            pals[m.group(1)] = m.group(2)
    return pals


def parse_metatiles(path):
    metatiles = OrderedDict()
    attributes = OrderedDict()
    for i, line in enumerate(open(path)):
        m = METATILES_RE.match(line.strip())
        if m:
            if m.group(1) in metatiles:
                sys.exit(f"FAIL: duplicate metatiles leaf {m.group(1)} "
                         f"(line {i + 1})")
            metatiles[m.group(1)] = m.group(2)
            continue
        m = ATTRIBUTES_RE.match(line.strip())
        if m:
            if m.group(1) in attributes:
                sys.exit(f"FAIL: duplicate attributes leaf {m.group(1)} "
                         f"(line {i + 1})")
            attributes[m.group(1)] = m.group(2)
    return metatiles, attributes


def parse_anim_frames(path):
    frames = OrderedDict()  # symbol -> (frame_index, [artifacts])
    tables = OrderedDict()  # table symbol -> [frame symbols]
    current_table = None
    table_frames = []
    floor_light_table = None
    floor_light_frames = []
    for i, line in enumerate(open(path)):
        stripped = line.strip()
        m = ANIM_FRAME_RE.match(stripped)
        if m:
            symbol = f"{m.group(1)}_Frame{m.group(2)}"
            if symbol in frames:
                sys.exit(f"FAIL: duplicate anim frame {symbol} (line {i + 1})")
            frames[symbol] = (int(m.group(2)), split_incbin_args(m.group(3)))
            continue
        m = ANIM_TABLE_RE.match(stripped)
        if m:
            if current_table is not None:
                sys.exit(f"FAIL: frame table {current_table} (line {i + 1}) "
                         "not terminated with };")
            name = m.group(1)
            if name in tables:
                sys.exit(f"FAIL: duplicate frame table {name} (line {i + 1})")
            current_table = name
            table_frames = []
            continue
        m = FLOOR_LIGHT_TABLE_RE.match(stripped)
        if m:
            if floor_light_table is not None:
                sys.exit(f"FAIL: duplicate floor-light table (line {i + 1})")
            floor_light_table = m.group(1)
            current_table = None
            table_frames = []
            floor_light_frames = []
            continue
        if current_table is not None:
            fr = FRAME_REF_RE.match(stripped)
            if fr:
                table_frames.append(fr.group(1))
                continue
            if stripped == "};":
                tables[current_table] = table_frames
                current_table = None
                table_frames = []
                continue
        if floor_light_table is not None:
            fr = FRAME_REF_RE.match(stripped)
            if fr:
                floor_light_frames.append(fr.group(1))
                continue
            if stripped == "};":
                floor_light_frames.sort()
                current_table = None
                floor_light_table = None
                floor_light_frames = []
                continue
    if current_table is not None:
        sys.exit(f"FAIL: frame table {current_table} not terminated with '}}';")
    return frames, tables


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv
    root = args[0] if len(args) > 0 else "."
    out = args[1] if len(args) > 1 else \
        "resources/extraction/emerald/bpee01/tileset/inventory.generated.toml"

    structs = parse_structs(f"{root}/{HEADERS_H}")
    tile_leaves = OrderedDict()
    for path in (GRAPHICS_H, GRAPHICS_C):
        tile_leaves.update(parse_tile_leaves(f"{root}/{path}"))
    palette_arrays = OrderedDict()
    for path in (GRAPHICS_H, GRAPHICS_C):
        palette_arrays.update(parse_palette_arrays(f"{root}/{path}"))
    floor_light_pals = parse_floor_light_pals(f"{root}/{GRAPHICS_C}")
    metatiles, attributes = parse_metatiles(f"{root}/{METATILES_H}")
    anim_frames, anim_tables = parse_anim_frames(f"{root}/{TILESET_ANIMS_C}")

    # ---- pin counts (hard failures) ----
    if len(structs) != PIN_STRUCTS:
        sys.exit(f"FAIL: {len(structs)} tileset structs (expected {PIN_STRUCTS})")
    n_primary = sum(1 for s in structs.values()
                    if s["isSecondary"] == "FALSE")
    if n_primary != PIN_PRIMARY:
        sys.exit(f"FAIL: {n_primary} primary tilesets (expected {PIN_PRIMARY})")

    referenced_tiles = {s["tiles"] for s in structs.values()}
    parity = [t for t in tile_leaves if t not in referenced_tiles]
    if len(parity) != PIN_PARITY_TILES or set(parity) != PARITY_TILES:
        sys.exit(f"FAIL: {len(parity)} unreferenced tile leaves "
                 f"(expected the {PIN_PARITY_TILES} documented GBA-parity "
                 "leaves): " + ", ".join(sorted(parity)))

    if len(palette_arrays) != PIN_PALETTE_ARRAYS:
        sys.exit(f"FAIL: {len(palette_arrays)} palette arrays "
                 f"(expected {PIN_PALETTE_ARRAYS})")
    if sum(len(r) for r in palette_arrays.values()) != PIN_PALETTE_ROWS:
        sys.exit(f"FAIL: palette rows != {PIN_PALETTE_ROWS}")

    if len(metatiles) != PIN_METATILES:
        sys.exit(f"FAIL: {len(metatiles)} metatiles leaves "
                 f"(expected {PIN_METATILES})")
    if len(attributes) != PIN_ATTRIBUTES:
        sys.exit(f"FAIL: {len(attributes)} attribute leaves "
                 f"(expected {PIN_ATTRIBUTES})")
    if len(anim_frames) != PIN_ANIM_FRAMES:
        sys.exit(f"FAIL: {len(anim_frames)} anim frames "
                 f"(expected {PIN_ANIM_FRAMES})")
    if len(anim_tables) != PIN_FRAME_TABLES:
        sys.exit(f"FAIL: {len(anim_tables)} frame tables "
                 f"(expected {PIN_FRAME_TABLES})")
    if len(floor_light_pals) != 4 or \
            sorted(floor_light_pals) != \
            [f"gTilesetAnims_BattleDomePals0_{i}" for i in range(4)]:
        sys.exit(f"FAIL: {len(floor_light_pals)} floor-light palette leaves "
                 "(expected exactly gTilesetAnims_BattleDomePals0_0..3)")

    # ---- symbol cross-checks (invalid symbols / missing consumers) ----
    for name, s in structs.items():
        for field in ("tiles", "palettes", "metatiles", "metatileAttributes"):
            ref = s[field]
            pool = {"tiles": tile_leaves, "palettes": palette_arrays,
                    "metatiles": metatiles,
                    "metatileAttributes": attributes}[field]
            if ref not in pool:
                sys.exit(f"FAIL: {name}.{field} references unknown symbol {ref}")
        if s["callback"] != "NULL" and \
                not s["callback"].startswith("InitTilesetAnim_"):
            sys.exit(f"FAIL: {name}.callback {s['callback']} is not an "
                     "InitTilesetAnim_* function or NULL")

    # Compression-flag consistency: isCompressed TRUE -> .lz artifact;
    # FALSE -> raw artifact. (GBA-parity leaves are not checked.)
    for name, s in structs.items():
        artifacts = tile_leaves[s["tiles"]]
        if len(artifacts) != 1:
            sys.exit(f"FAIL: {s['tiles']} has {len(artifacts)} artifacts "
                     "(tiles must be a single INCBIN)")
        if s["isCompressed"] == "TRUE" and not artifacts[0].endswith(".lz"):
            sys.exit(f"FAIL: {name} isCompressed=TRUE but {s['tiles']} "
                     f"artifact {artifacts[0]} is not .lz")
        if s["isCompressed"] == "FALSE" and artifacts[0].endswith(".lz"):
            sys.exit(f"FAIL: {name} isCompressed=FALSE but {s['tiles']} "
                     f"artifact {artifacts[0]} is .lz")

    # Metatile/attribute pairing: the same canonical suffix must appear in
    # both leaf sets, and the artifact pairs must correspond 1:1.
    met_suffix = {canonical_suffix(m, 'gMetatiles_'): m for m in metatiles}
    att_suffix = {canonical_suffix(a, 'gMetatileAttributes_'): a for a in attributes}
    if set(met_suffix) != set(att_suffix):
        sys.exit("FAIL: metatiles/attributes canonical sets differ: "
                 + ", ".join(sorted(set(met_suffix) ^ set(att_suffix))))

    # Aliasing: distinct symbols sharing one canonical must share identical
    # artifacts; anything else is inconsistent aliasing.
    def resource_key(kind, canonical):
        return (kind, canonical)

    resources = OrderedDict()  # (kind, canonical) -> list of symbols
    for symbol, s in structs.items():
        for kind, field, prefix in (("tiles", "tiles", "gTilesetTiles_"),
                                     ("palettes", "palettes", "gTilesetPalettes_"),
                                     ("metatiles", "metatiles", "gMetatiles_"),
                                     ("metatile_attributes", "metatileAttributes",
                                      "gMetatileAttributes_")):
            ref = s[field]
            canonical = canonical_suffix(ref, prefix)
            resources.setdefault(resource_key(kind, canonical), []).append(
                (symbol, ref))
    # Add anim frames.
    for symbol, (frame_no, artifacts) in anim_frames.items():
        anim = canonical_suffix(symbol[:-len(f"_Frame{frame_no}")],
                                       "gTilesetAnims_")
        resources.setdefault(resource_key("anim_frame", f"{anim}/{frame_no}"),
                             []).append((symbol, symbol))

    for (kind, canonical), consumers in resources.items():
        symbols = {ref for (_, ref) in consumers}
        if len(symbols) < 2:
            continue
        # Same canonical from multiple symbols: verify identical artifacts.
        if kind == "tiles":
            artifacts_by_symbol = {ref: tuple(tile_leaves[ref])
                                   for ref in symbols}
        elif kind == "palettes":
            artifacts_by_symbol = {ref: tuple(palette_arrays[ref])
                                   for ref in symbols}
        elif kind == "metatiles":
            artifacts_by_symbol = {ref: (metatiles[ref],) for ref in symbols}
        elif kind == "metatile_attributes":
            artifacts_by_symbol = {ref: (attributes[ref],) for ref in symbols}
        else:
            continue  # anim frames cannot alias (canonical embeds frame no)
        if len(set(artifacts_by_symbol.values())) != 1:
            sys.exit(f"FAIL: inconsistent aliasing at canonical "
                     f"tileset/{canonical}: " +
                     ", ".join(f"{s}: {a}"
                               for s, a in sorted(artifacts_by_symbol.items())))

    # Frame-leaf consumer check: leaves without a frame-table consumer are
    # GBA_PARITY (no canonical id), exactly like the unreferenced tile
    # leaves. Pinned: 10 (Lavaridge_Cave_Lava Frame4-7, Unused1 Frame0-3,
    # Unused2 Frame0-1) - tileset_anims.c:329-332/497-500/523-526 have no
    # table/callback/queue reference.
    referenced_frames = set()
    for frames in anim_tables.values():
        referenced_frames.update(frames)
    parity_frames = [f for f in anim_frames if f not in referenced_frames]
    if len(parity_frames) != PIN_ANIM_PARITY:
        sys.exit(f"FAIL: {len(parity_frames)} unreferenced anim frames "
                 f"(expected {PIN_ANIM_PARITY}): "
                 + ", ".join(sorted(parity_frames)))

    # ---- emit ----
    lines = []
    lines.append("# Generated by tools/gen3_resources/tileset_family/"
                 "gen_tileset_inventory.py")
    lines.append("# Do not edit by hand; edit the checked-in tileset data "
                 "headers and re-run the generator (regeneration must be a "
                 "no-op diff).")
    lines.append("inventory_version = 1")
    lines.append('family = "tileset"')
    lines.append('game = "emerald"')
    lines.append('rom_profile = "bpee01"')
    lines.append("")

    lines.append("[[families]]")
    lines.append("kind = \"struct\"")
    lines.append(f"symbol_count = {len(structs)}")
    lines.append("")
    lines.append("[[families]]")
    lines.append("kind = \"tile\"")
    lines.append(f"symbol_count = {len(referenced_tiles)}")
    lines.append("")
    lines.append("[[families]]")
    lines.append("kind = \"palette_array\"")
    lines.append(f"symbol_count = {len(palette_arrays)}")
    lines.append(f"row_count = {PIN_PALETTE_ROWS}")
    lines.append("")
    lines.append("[[families]]")
    lines.append("kind = \"metatiles\"")
    lines.append(f"symbol_count = {len(metatiles)}")
    lines.append("")
    lines.append("[[families]]")
    lines.append("kind = \"metatile_attributes\"")
    lines.append(f"symbol_count = {len(attributes)}")
    lines.append("")
    lines.append("[[families]]")
    lines.append("kind = \"anim_frame\"")
    lines.append(f"symbol_count = {len(anim_frames)}")
    lines.append(f"parity_count = {len(parity_frames)}")
    lines.append("")
    lines.append("[[families]]")
    lines.append("kind = \"floor_light_pal\"")
    lines.append(f"symbol_count = {len(floor_light_pals)}")
    lines.append("")

    for name in sorted(structs):
        s = structs[name]
        lines.append("[[structs]]")
        lines.append(f'symbol = "{name}"')
        lines.append(f"is_compressed = {'true' if s['isCompressed'] == 'TRUE' else 'false'}")
        lines.append(f"is_secondary = {'true' if s['isSecondary'] == 'TRUE' else 'false'}")
        lines.append(f'tiles = "{s["tiles"]}"')
        lines.append(f'palettes = "{s["palettes"]}"')
        lines.append(f'metatiles = "{s["metatiles"]}"')
        lines.append(f'metatile_attributes = "{s["metatileAttributes"]}"')
        lines.append(f'callback = "{s["callback"]}"')
        lines.append("")

    for symbol in sorted(tile_leaves):
        artifacts = tile_leaves[symbol]
        lines.append("[[resources]]")
        lines.append(f'symbol = "{symbol}"')
        lines.append('kind = "tile"')
        lines.append(f'canonical = "{canonical_suffix(symbol, "gTilesetTiles_")}"')
        lines.append(f'parity = {"true" if symbol in parity else "false"}')
        if len(artifacts) == 1:
            lines.append(f'source_artifact = "{artifacts[0]}"')
        else:
            lines.append(f'source_artifact = "{artifacts[0]}"')
            for extra in artifacts[1:]:
                lines.append(f'source_artifact_2 = "{extra}"')
        lines.append("")

    for symbol in sorted(palette_arrays):
        rows = palette_arrays[symbol]
        lines.append("[[resources]]")
        lines.append(f'symbol = "{symbol}"')
        lines.append('kind = "palette_array"')
        lines.append(f'canonical = "{canonical_suffix(symbol, "gTilesetPalettes_")}"')
        lines.append("")
        for idx, artifact in enumerate(rows):
            lines.append("[[palette_rows]]")
            lines.append(f'array = "{symbol}"')
            lines.append(f"row = {idx}")
            lines.append(f'source_artifact = "{artifact}"')
            lines.append("")

    for symbol in sorted(metatiles):
        lines.append("[[resources]]")
        lines.append(f'symbol = "{symbol}"')
        lines.append('kind = "metatiles"')
        lines.append(f'canonical = "{canonical_suffix(symbol, "gMetatiles_")}"')
        lines.append(f'source_artifact = "{metatiles[symbol]}"')
        lines.append("")

    for symbol in sorted(attributes):
        lines.append("[[resources]]")
        lines.append(f'symbol = "{symbol}"')
        lines.append('kind = "metatile_attributes"')
        lines.append(f'canonical = "{canonical_suffix(symbol, "gMetatileAttributes_")}"')
        lines.append(f'source_artifact = "{attributes[symbol]}"')
        lines.append("")

    for symbol in sorted(anim_frames):
        frame_no, artifacts = anim_frames[symbol]
        anim = canonical_suffix(symbol[:-len(f"_Frame{frame_no}")],
                                       "gTilesetAnims_")
        if symbol in parity_frames:
            lines.append("[[parity_frames]]")
            lines.append(f'symbol = "{symbol}"')
            lines.append("")
            continue
        lines.append("[[resources]]")
        lines.append(f'symbol = "{symbol}"')
        lines.append('kind = "anim_frame"')
        lines.append(f'canonical = "{anim}"')
        lines.append(f"frame = {frame_no}")
        if len(artifacts) == 1:
            lines.append(f'source_artifact = "{artifacts[0]}"')
        else:
            lines.append(f'source_artifact = "{artifacts[0]}"')
            for extra in artifacts[1:]:
                lines.append(f'source_artifact_2 = "{extra}"')
        lines.append("")

    for symbol in sorted(floor_light_pals):
        lines.append("[[resources]]")
        lines.append(f'symbol = "{symbol}"')
        lines.append('kind = "floor_light_pal"')
        lines.append(f'source_artifact = "{floor_light_pals[symbol]}"')
        lines.append("")

    for table in sorted(anim_tables):
        lines.append("[[frame_tables]]")
        lines.append(f'symbol = "{table}"')
        lines.append(f"frame_count = {len(anim_tables[table])}")
        lines.append("")
        for frame in anim_tables[table]:
            lines.append("[[consumers]]")
            lines.append(f'table = "{table}"')
            lines.append(f'frame = "{frame}"')
            lines.append("")

    text = "\n".join(lines)
    if check:
        try:
            existing = open(out).read()
        except FileNotFoundError:
            sys.exit(f"FAIL: --check: {out} does not exist")
        if existing != text:
            sys.exit(f"FAIL: --check: {out} is not up to date with the sources")
        print(f"{out}: up to date ({len(structs)} structs, "
              f"{len(referenced_tiles)} tiles + {len(parity)} parity, "
              f"{len(palette_arrays)} palette arrays ({PIN_PALETTE_ROWS} rows), "
              f"{len(metatiles)} metatiles, {len(attributes)} attributes, "
              f"{len(anim_frames)} anim frames, {len(anim_tables)} frame tables)")
        return 0
    with open(out, "w") as f:
        f.write(text)
    print(f"{out}: wrote {len(structs)} structs, "
          f"{len(referenced_tiles)} tiles + {len(parity)} parity, "
          f"{len(palette_arrays)} palette arrays ({PIN_PALETTE_ROWS} rows), "
          f"{len(metatiles)} metatiles, {len(attributes)} attributes, "
          f"{len(anim_frames)} anim frames, {len(floor_light_pals)} "
          f"floor-light pals, {len(anim_tables)} frame tables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
