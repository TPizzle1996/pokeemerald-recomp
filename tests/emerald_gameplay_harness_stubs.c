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

/* ---- R13-D2: item-use engine functions. The offline harness does not
 * compile src/item_use.c; these stubs let gameplay_item_callbacks_native.c
 * link so the seam's callback resolution + table can be exercised against
 * the real pack. Each stub is a distinct non-NULL function so pointer-
 * identity checks in the item battery are meaningful. ---- */
void ItemUseOutOfBattle_Mail(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_Bike(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_Rod(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_Itemfinder(uint8_t var) { (void)var; }
void ItemUseOutOfBattle_PokeblockCase(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_CoinCase(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_PowderJar(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_WailmerPail(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_Medicine(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_ReduceEV(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_SacredAsh(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_PPRecovery(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_PPUp(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_RareCandy(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_TMHM(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_Repel(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_EscapeRope(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_BlackWhiteFlute(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_EvolutionStone(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_Berry(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_EnigmaBerry(uint8_t taskId) { (void)taskId; }
void ItemUseOutOfBattle_CannotUse(uint8_t taskId) { (void)taskId; }
void ItemUseInBattle_PokeBall(uint8_t taskId) { (void)taskId; }
void ItemUseInBattle_StatIncrease(uint8_t taskId) { (void)taskId; }
void ItemUseInBattle_Medicine(uint8_t taskId) { (void)taskId; }
void ItemUseInBattle_PPRecovery(uint8_t taskId) { (void)taskId; }
void ItemUseInBattle_Escape(uint8_t taskId) { (void)taskId; }
void ItemUseInBattle_EnigmaBerry(uint8_t taskId) { (void)taskId; }
void Task_UseDigEscapeRopeOnField(uint8_t taskId) { (void)taskId; }