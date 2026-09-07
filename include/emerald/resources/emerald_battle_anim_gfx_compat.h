#ifndef EMERALD_RESOURCES_EMERALD_BATTLE_ANIM_GFX_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_BATTLE_ANIM_GFX_COMPAT_H

/* R15 Phase 4: Emerald battle-animation graphics native compatibility
 * publication. Graphics only - the H VM bytecode family is unchanged.
 *
 * Publishes the five battle-animation gfx tables from the ROM_BASE pack:
 *   gBattleAnimPicTable
 *   gBattleAnimPaletteTable
 *   gBattleAnimBackgroundTable
 *   sBallParticleSpriteSheets
 *   sBattleEnvironmentTable
 *
 * The individual compiled symbols (gBattleAnimSpriteGfx_*, etc.) are
 * retained on native for the remaining direct consumers; the tables
 * are published from the pack at init.
 */

#include <stdint.h>
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

enum EmeraldResourceCompatStatus
EmeraldBattleAnimGfxCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

enum EmeraldResourceCompatStatus
EmeraldBattleAnimGfxCompat_Republish(
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldBattleAnimGfxCompat_ClearMigratedEntries(void);
void EmeraldBattleAnimGfxCompat_Shutdown(void);

const struct EmeraldResourceCompatibilityImage *EmeraldBattleAnimGfxCompat_GetImage(void);
uint32_t EmeraldBattleAnimGfxCompat_GetEntrySchema(size_t entryIndex);
uint32_t EmeraldBattleAnimGfxCompat_GetEntryRole(size_t entryIndex);

const void *BattleAnimGfx_Get(const char *id);

#endif