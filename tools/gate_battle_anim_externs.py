#!/usr/bin/env python3
"""Gate battle-anim extern declarations in graphics.h with DESKTOP_EXTERNAL_GAME_CONTENT."""
import re
import sys

# Read the accessor header to get the list of symbols to gate
with open('include/emerald/resources/battle_anim_gfx_accessors.generated.h') as f:
    accessor = f.read()
gated_symbols = set()
for line in accessor.split('\n'):
    m = re.match(r'#define (\w+) BattleAnimGfx_Get', line)
    if m:
        gated_symbols.add(m.group(1))

print(f"Found {len(gated_symbols)} symbols to gate")

# Read graphics.h
with open('include/graphics.h') as f:
    lines = f.readlines()

# Find contiguous ranges of lines that need gating
gated_indices = set()
for i, line in enumerate(lines):
    m = re.match(r'extern const \w+ (\w+)\[\];', line)
    if m and m.group(1) in gated_symbols:
        gated_indices.add(i)

# Also check for lines that are already inside a #ifndef block
# Build contiguous ranges
ranges = []
start = None
for i in sorted(gated_indices):
    if start is None:
        start = i
    elif i > start + 1 and not all(j in gated_indices for j in range(start, i+1)):
        # Check if the gap is just non-gated externs or comments
        gap_lines = [lines[j].strip() for j in range(start+1, i)]
        gap_is_trivial = all(
            not l or l.startswith('//') or l.startswith('/*') or l.startswith('*') or l == '*/'
            for l in gap_lines
        )
        if not gap_is_trivial:
            ranges.append((start, i-1))
            start = i
    # else continue the range

if start is not None:
    # Extend to include trailing non-extern lines at the end of a block
    end = start
    for i in range(start, len(lines)):
        if i in gated_indices:
            end = i
    ranges.append((start, end))

# Merge overlapping/adjacent ranges
merged = []
for r in sorted(ranges):
    if not merged:
        merged.append(r)
    else:
        prev_start, prev_end = merged[-1]
        if r[0] <= prev_end + 1:
            merged[-1] = (prev_start, max(prev_end, r[1]))
        else:
            merged.append(r)

print(f"Found {len(merged)} contiguous ranges to gate:")
for s, e in merged:
    print(f"  Lines {s+1}-{e+1}")

# Now gate each range
# We need to be careful: if a range starts mid-block, we need to split it properly
# Actually, let's use a simpler approach: gate each individual line

# But individual gating is ugly. Let's try a different approach:
# Find CONTIGUOUS blocks of extern declarations that are ALL gated

# Re-scan: find maximal contiguous blocks where ALL extern declarations are gated
blocks = []
block_start = None
for i in range(len(lines)):
    line = lines[i]
    m = re.match(r'extern const \w+ (\w+)\[\];', line)
    if m:
        if m.group(1) in gated_symbols:
            if block_start is None:
                block_start = i
        else:
            if block_start is not None:
                blocks.append((block_start, i-1))
                block_start = None
    elif block_start is not None:
        # Non-extern line inside a block - check if it's a comment or blank
        stripped = line.strip()
        if stripped and not stripped.startswith('//') and not stripped.startswith('/*') and not stripped.startswith('*') and stripped != '*/':
            blocks.append((block_start, i-1))
            block_start = None

if block_start is not None:
    blocks.append((block_start, len(lines)-1))

print(f"\nContiguous gated extern blocks: {len(blocks)}")
for s, e in blocks:
    count = sum(1 for i in range(s, e+1) if re.match(r'extern const \w+ \w+\[\];', lines[i]) and 
                re.match(r'extern const \w+ (\w+)\[\];', lines[i]).group(1) in gated_symbols)
    print(f"  Lines {s+1}-{e+1} ({count} gated symbols)")

# Now apply the gating
# We need to process from bottom to top to preserve line numbers
out_lines = []
i = 0
while i < len(lines):
    # Check if this line starts a gated block
    in_block = False
    for s, e in blocks:
        if i == s:
            in_block = True
            # Add the #ifndef gate
            out_lines.append('#ifndef DESKTOP_EXTERNAL_GAME_CONTENT\n')
            # Add all lines in the block
            for j in range(s, e+1):
                out_lines.append(lines[j])
            # Add the #endif
            out_lines.append('#endif /* !DESKTOP_EXTERNAL_GAME_CONTENT */\n')
            i = e + 1
            break
    if not in_block:
        out_lines.append(lines[i])
        i += 1

with open('include/graphics.h', 'w') as f:
    f.writelines(out_lines)

print(f"\nGated {sum(1 for s,e in blocks for i in range(s,e+1) if re.match(r'extern const \w+ \w+\[\];', lines[i]) and re.match(r'extern const \w+ (\w+)\[\];', lines[i]).group(1) in gated_symbols)} extern declarations in {len(blocks)} blocks")