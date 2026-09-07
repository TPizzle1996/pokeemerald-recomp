#ifndef EMERALD_RESOURCES_EMERALD_WEATHER_COMPAT_H
#define EMERALD_RESOURCES_EMERALD_WEATHER_COMPAT_H

/* R15: weather data native compatibility publication.
 * Publishes the 49,152-byte drought-color lookup table (six 0x1000-u16
 * rows, field_weather.c's sDroughtWeatherColors) from the pack. */

#include <stdint.h>
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"

enum EmeraldResourceCompatStatus
EmeraldWeatherCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics);

void EmeraldWeatherCompat_ClearMigratedEntries(void);

#endif
