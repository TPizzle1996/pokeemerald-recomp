#!/usr/bin/env python3
"""Gate item icon INCBIN definitions in items.h with DESKTOP_EXTERNAL_GAME_CONTENT.
Keeps QuestionMark and ReturnToFieldArrow symbols compiled (shared fallbacks)."""
import re

with open('src/data/graphics/items.h') as f:
    lines = f.readlines()

KEEP_COMPILED = {'gItemIcon_QuestionMark', 'gItemIconPalette_QuestionMark',
                 'gItemIcon_ReturnToFieldArrow', 'gItemIconPalette_ReturnToFieldArrow'}

out = []
for line in lines:
    m = re.match(r'const u32 (gItemIcon\w+)\[\] = INCBIN', line)
    if m and m.group(1) not in KEEP_COMPILED:
        out.append('#ifndef DESKTOP_EXTERNAL_GAME_CONTENT\n')
        out.append(line)
        out.append('#endif /* DESKTOP_EXTERNAL_GAME_CONTENT */\n')
    else:
        out.append(line)

with open('src/data/graphics/items.h', 'w') as f:
    f.writelines(out)

gated = sum(1 for l in out if 'DESKTOP_EXTERNAL_GAME_CONTENT' in l)
print(f"Gated {gated} lines, kept {len(KEEP_COMPILED)} symbols compiled")