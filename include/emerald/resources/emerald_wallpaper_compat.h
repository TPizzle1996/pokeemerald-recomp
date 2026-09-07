#ifndef EMERALD_RESOURCES_EMERALD_WALLPAPER_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_WALLPAPER_COMPAT_H

/* R15 Phase 6: Emerald wallpaper graphics native compatibility
 * publication. Publishes the wallpaper tables (sWallpapers,
 * sWaldaWallpapers, sWaldaWallpaperIcons, sArrow_Gfx) from the
 * ROM_BASE pack.
 */

#include <stdint.h>
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

enum EmeraldResourceCompatStatus
EmeraldWallpaperCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldWallpaperCompat_ClearMigratedEntries(void);

const void *Wallpaper_Get(const char *id);

#endif