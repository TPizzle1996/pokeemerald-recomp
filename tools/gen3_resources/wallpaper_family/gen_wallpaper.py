#!/usr/bin/env python3
"""R15 Phase 6: wallpaper graphics family generator."""

import re, os, hashlib, sys

def manifest_map(family_dir):
    """id -> canonical_decoded_sha256 from the production manifest."""
    p = f"resources/extraction/emerald/bpee01/{family_dir}/manifest.production.toml"
    out = {}
    try:
        text = open(p).read()
        for block in text.split('[[records]]')[1:]:
            m_id = re.search(r'id = "([^"]+)"', block)
            m_sha = re.search(r'canonical_decoded_sha256 = "([0-9a-f]+)"', block)
            if m_id and m_sha:
                out[m_id.group(1)] = m_sha.group(1)
    except OSError:
        pass
    return out


def incbin_symbols(path):
    out = []
    pat = re.compile(r'(?:static\s+)?const\s+\w+\s+(\w+)\[\]\s*=\s*INCBIN_\w+\("([^"]+)"\)')
    for line in open(path):
        m = pat.search(line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out

def parse_palette_arrays(path):
    out = []
    pat_decl = re.compile(r'static\s+const\s+u16\s+(s\w*Palettes?\w*_\w+)\[\]\[16\]\s*=')
    pat_inc = re.compile(r'INCBIN_U16\("([^"]+)"\)')
    in_array = False
    cur_name = None
    cur_incs = []
    for line in open(path):
        if in_array:
            m = pat_inc.search(line)
            if m:
                cur_incs.append(m.group(1))
            if '};' in line:
                if len(cur_incs) >= 2:
                    out.append((cur_name, cur_incs[0], cur_incs[1]))
                elif len(cur_incs) == 1:
                    out.append((cur_name, cur_incs[0], cur_incs[0]))
                in_array = False
                cur_name = None
                cur_incs = []
        else:
            m = pat_decl.search(line)
            if m:
                cur_name = m.group(1)
                in_array = True
                cur_incs = []
    return out

def name_from_sym(sym):
    m = re.match(r'[sg]Wallpaper(Tiles|Tilemap|Palettes?)_(\w+)', sym)
    return m.group(2) if m else None

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    srcdir = f"{root}/src"
    outdir = f"{root}/resources/extraction/emerald/bpee01/wallpaper"
    os.makedirs(outdir, exist_ok=True)

    all_syms = incbin_symbols(f"{srcdir}/data/wallpapers.h") + incbin_symbols(f"{srcdir}/graphics.c")
    pal_arrays = parse_palette_arrays(f"{srcdir}/data/wallpapers.h") + parse_palette_arrays(f"{srcdir}/graphics.c")

    resources = []
    for sym, art in all_syms:
        wp = 'wallpaper' in art.lower() or 'wallpaper' in sym.lower()
        if not wp and sym not in ('sArrow_Gfx', 'sWallpaperTilemap_Unused'):
            continue
        if 'Icon' in sym:
            slug = sym.replace('gWallpaperIcon_', '').replace('sWallpaperIcon_', '')
            resources.append((f'emerald:wallpaper-icon/{slug.lower()}', 'tile-graphics', 'gba-4bpp-tiles', sym, art))
        elif sym == 'sWallpaperTilemap_Unused':
            resources.append(('emerald:wallpaper/misc/unused-tilemap', 'binary', 'gba-bytes', sym, art))
        elif sym == 'sArrow_Gfx':
            resources.append(('emerald:wallpaper/misc/arrow', 'binary', 'gba-bytes', sym, art))
        else:
            name = name_from_sym(sym)
            if name:
                if 'Tiles' in sym and 'Tilemap' not in sym:
                    resources.append((f'emerald:wallpaper/{name.lower()}/tiles', 'tile-graphics', 'gba-4bpp-tiles', sym, art))
                elif 'Tilemap' in sym:
                    resources.append((f'emerald:wallpaper/{name.lower()}/tilemap', 'binary', 'gba-bytes', sym, art))

    for sym, frame_art, bg_art in pal_arrays:
        name = name_from_sym(sym)
        if name:
            # Palette arrays are 2D: the ELF symbol covers the full array but
            # each resource is a single palette. Use the ROM offset fallback
            # by giving each palette a unique synthetic symbol name not in the
            # ELF. The manifest tool will locate the bytes in the ROM instead.
            fsym = f"{sym}_frame"
            bsym = f"{sym}_bg"
            resources.append((f'emerald:wallpaper/{name.lower()}/frame-pal', 'palette', 'gba-bgr555-palette', fsym, frame_art))
            resources.append((f'emerald:wallpaper/{name.lower()}/bg-pal', 'palette', 'gba-bgr555-palette', bsym, bg_art))

    seen = set()
    uniq = []
    for r in resources:
        if r[0] not in seen:
            seen.add(r[0])
            uniq.append(r)
    resources = uniq
    print(f'Total resources: {len(resources)}')
    tiles = len([r for r in resources if '/tiles' in r[0] and 'tilemap' not in r[0]])
    tilemaps = len([r for r in resources if '/tilemap' in r[0]])
    frame_pals = len([r for r in resources if '/frame-pal' in r[0]])
    bg_pals = len([r for r in resources if '/bg-pal' in r[0]])
    icons = len([r for r in resources if 'wallpaper-icon' in r[0]])
    misc = len([r for r in resources if 'misc' in r[0]])
    print(f'  tiles: {tiles}, tilemaps: {tilemaps}, frame_pals: {frame_pals}, bg_pals: {bg_pals}, icons: {icons}, misc: {misc}')

    # catalog
    canon = manifest_map("wallpaper") or {}
    cat = ['# Generated by tools/gen3_resources/wallpaper_family/gen_wallpaper.py',
           '# Do not edit by hand; regeneration must be a no-op diff.',
           'catalog_version = 1', 'namespace = "emerald"',
           'resource_api = "1.0.0"',
           'game = "emerald"', 'rom_profile = "bpee01-rev0"', '']
    for rid, rtype, _, _, _ in resources:
        cat.append('[[resources]]')
        cat.append(f'id = "{rid}"')
        cat.append(f'type = "{rtype}"')
        cat.append('schema = 1')
        cat.append('required_for_base = true')
    with open(f'{outdir}/catalog.generated.toml', 'w') as f:
        f.write('\n'.join(cat) + '\n')

    # bindings
    bin_ = ['# Generated by tools/gen3_resources/wallpaper_family/gen_wallpaper.py',
            '# Do not edit by hand; regeneration must be a no-op diff.',
            'bindings_version = 1', 'game = "emerald"',
            'rom_profile = "bpee01-rev0"', '']
    for rid, rtype, canon_rep, sym, art in resources:
        data = open(f'{root}/{art}', 'rb').read()
        if art.endswith('.lz') and data[0] & 0x0F == 0 and len(data) >= 4:
            enc = 'gba-lz77'
            dec = data[1] | (data[2] << 8) | (data[3] << 16)
        else:
            enc = 'raw'
            dec = len(data)
        bin_.append('[[bindings]]')
        bin_.append(f'id = "{rid}"')
        bin_.append(f'symbol = "{sym}"')
        bin_.append(f'source_artifact = "{art}"')
        bin_.append(f'source_encoding = "{enc}"')
        bin_.append(f'canonical_representation = "{canon_rep}"')
        bin_.append(f'expected_decoded_size = {dec}')
        if '-pal' in rid:
            bin_.append('allow_shared_range = true')
    with open(f'{outdir}/bindings.generated.toml', 'w') as f:
        f.write('\n'.join(bin_) + '\n')

    # ownership
    own = ['# Generated by tools/gen3_resources/wallpaper_family/gen_wallpaper.py',
           '# Do not edit by hand; regeneration must be a no-op diff.',
           'ownership_version = 1', 'game = "emerald"',
           'rom_profile = "bpee01-rev0"', '']
    for rid, rtype, canon_rep, sym, art in resources:
        key = hashlib.sha256(b'gen3-resource-id-v1\x00' + rid.encode()).hexdigest()
        data = open(f'{root}/{art}', 'rb').read()
        if art.endswith('.lz') and data[0] & 0x0F == 0 and len(data) >= 4:
            enc = 'gba-lz77'
            dec = data[1] | (data[2] << 8) | (data[3] << 16)
        else:
            enc = 'raw'
            dec = len(data)
        src_sha = hashlib.sha256(data).hexdigest()
        own.append('[[resources]]')
        own.append(f'id = "{rid}"')
        own.append(f'key = "{key}"')
        own.append(f'legacy_symbol = "{sym}"')
        own.append(f'type = "{rtype}"')
        own.append('schema = 1')
        own.append(f'source_artifact = "{art}"')
        own.append(f'encoded_length = {len(data)}')
        own.append(f'decoded_length = {dec}')
        own.append(f'source_encoding = "{enc}"')
        own.append(f'source_encoded_sha256 = "{src_sha}"')
        own.append(f'canonical_decoded_sha256 = "{canon.get(rid, "")}"')
        own.append('ownership_state = "ROM_BASE_ONLY"')
        own.append('')
        own.append('[resources.targets]')
        own.append('native = "ROM_BASE_ONLY"')
        own.append('gba = "COMPILED"')
    with open(f'{outdir}/ownership.generated.toml', 'w') as f:
        f.write('\n'.join(own) + '\n')

    # inventory
    inv = ['# Generated by tools/gen3_resources/wallpaper_family/gen_wallpaper.py',
           '# Do not edit by hand; regeneration must be a no-op diff.',
           'inventory_version = 1',
           'family = "wallpaper"',
           'game = "emerald"',
           'rom_profile = "bpee01-rev0"', '']
    for rid, rtype, canon_rep, sym, art in resources:
        inv.append('[[resources]]')
        inv.append(f'symbol = "{sym}"')
        inv.append(f'source_artifact = "{art}"')
        inv.append(f'resource_id = "{rid}"')
    with open(f'{outdir}/inventory.generated.toml', 'w') as f:
        f.write('\n'.join(inv) + '\n')

    # slot map
    idx_of = {}
    idx = 0
    for rid, _, _, _, _ in resources:
        idx_of[rid] = idx
        idx += 1

    h = ['/* Generated by tools/gen3_resources/wallpaper_family/gen_wallpaper.py',
         ' * Do not edit by hand; regeneration must be a no-op diff.',
         ' *',
         ' * R15 Phase 6: native compat slot map for wallpaper graphics.',
         ' */',
         '#ifndef EMERALD_RESOURCES_WALLPAPER_SLOTS_GENERATED_H',
         '#define EMERALD_RESOURCES_WALLPAPER_SLOTS_GENERATED_H',
         '',
         '#include <stdint.h>',
         '#include "gen3/resources/resource_types.h"',
         '#include "emerald/resources/emerald_resource_ranges.h"',
         '']
    h.append(f'#define WALLPAPER_RESOURCE_COUNT {len(resources)}u')
    h.append('')
    h.append('struct WallpaperResource {')
    h.append('    const char *id;')
    h.append('    uint32_t expectedSize;')
    h.append('    enum Gen3ResourceType type;')
    h.append('    uint32_t schema;')
    h.append('    uint32_t role;')
    h.append('};')
    h.append('')
    h.append('static const struct WallpaperResource')
    h.append(f'    kWallpaperResources[WALLPAPER_RESOURCE_COUNT] =')
    h.append('{')
    for rid, rtype, canon_rep, sym, art in resources:
        data = open(f'{root}/{art}', 'rb').read()
        if art.endswith('.lz') and data[0] & 0x0F == 0 and len(data) >= 4:
            dec = data[1] | (data[2] << 8) | (data[3] << 16)
        else:
            dec = len(data)
        ctype = 'GEN3_RESOURCE_TYPE_TILE_GRAPHICS' if rtype == 'tile-graphics' else \
                'GEN3_RESOURCE_TYPE_PALETTE' if rtype == 'palette' else \
                'GEN3_RESOURCE_TYPE_BINARY'
        h.append(f'    {{ "{rid}", {dec}u, {ctype}, 1u, EMERALD_RESOURCE_ROLE_LEGACY_LZ }},')
    h.append('};')
    h.append('')
    h.append('#endif')
    with open(f'{root}/include/emerald/resources/wallpaper_slots.generated.h', 'w') as f:
        f.write('\n'.join(h) + '\n')
    print('slot map emitted')

    # accessor macros
    a = ['/* Generated by tools/gen3_resources/wallpaper_family/gen_wallpaper.py',
         ' * Do not edit by hand; regeneration must be a no-op diff.',
         ' *',
         ' * R15 Phase 6: on native every legacy wallpaper symbol name redirects',
         ' * to the compat seam accessor; on GBA the names stay the real INCBIN',
         ' * arrays. */']
    a.append('#if defined(NATIVE_LINUX)')
    a.append('#include "emerald/resources/emerald_wallpaper_compat.h"')
    for rid, _, _, sym, art in resources:
        data = open(f"{root}/{art}", "rb").read()
        if art.endswith('.lz') and data[0] & 0x0F == 0 and len(data) >= 4:
            dec = data[1] | (data[2] << 8) | (data[3] << 16)
        else:
            dec = len(data)
        a.append(f'#define {sym} Wallpaper_Get("{rid}")')
        a.append(f'#define {sym}_SIZE {dec}u')
    a.append('#endif')
    with open(f'{root}/include/emerald/resources/wallpaper_accessors.generated.h', 'w') as f:
        f.write('\n'.join(a) + '\n')
    print('accessor macros emitted')

if __name__ == '__main__':
    main()