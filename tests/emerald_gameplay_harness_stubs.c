/* R13-D1: gameplay harness font-glyph stubs.
 *
 * The ten u16 font glyph arrays are normally HOST_DATA in src/fonts.c, which
 * the offline test harness does not compile. This stub provides the same
 * symbols (extern u16, sizes matching the generated gameplay table) so the
 * gameplay seam (emerald_gameplay_compat.c) can be linked into the harness
 * and its font fills exercised against the production pack.
 */

#include <stdint.h>

#define ALIGNED(n) __attribute__((aligned(n)))

extern uint16_t gFontSmallNarrowLatinGlyphs[16384];
extern uint16_t gFontSmallLatinGlyphs[16384];
extern uint16_t gFontNarrowLatinGlyphs[16384];
extern uint16_t gFontShortLatinGlyphs[16384];
extern uint16_t gFontNormalLatinGlyphs[16384];
extern uint16_t gFontSmallJapaneseGlyphs[8192];
extern uint16_t gFontNormalJapaneseGlyphs[8192];
extern uint16_t gFontFRLGMaleJapaneseGlyphs[16384];
extern uint16_t gFontFRLGFemaleJapaneseGlyphs[16384];
extern uint16_t gFontShortJapaneseGlyphs[16384];

ALIGNED(4) uint16_t gFontSmallNarrowLatinGlyphs[16384];
ALIGNED(4) uint16_t gFontSmallLatinGlyphs[16384];
ALIGNED(4) uint16_t gFontNarrowLatinGlyphs[16384];
ALIGNED(4) uint16_t gFontShortLatinGlyphs[16384];
ALIGNED(4) uint16_t gFontNormalLatinGlyphs[16384];
ALIGNED(4) uint16_t gFontSmallJapaneseGlyphs[8192];
ALIGNED(4) uint16_t gFontNormalJapaneseGlyphs[8192];
ALIGNED(4) uint16_t gFontFRLGMaleJapaneseGlyphs[16384];
ALIGNED(4) uint16_t gFontFRLGFemaleJapaneseGlyphs[16384];
ALIGNED(4) uint16_t gFontShortJapaneseGlyphs[16384];