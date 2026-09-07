/* R15: weather data native compatibility publication.
 * See emerald_weather_compat.h for the contract. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_weather_compat.h"

/* R15: the six-row drought color lookup table (field_weather.c) is mutable
 * on native and filled here from the pack. */
extern u16 sDroughtWeatherColors[6][0x1000];

enum EmeraldResourceCompatStatus
EmeraldWeatherCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    static const char *const kWeatherId = "emerald:data/weather/drought-colors";
    Gen3ResourceHandle handle;
    struct Gen3ResourceView view;
    enum Gen3ResourceResult result;
    enum EmeraldResourceCompatStatus status;

    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diagnostics != NULL)
    {
        snprintf(diagnostics->canonicalName,
                 sizeof(diagnostics->canonicalName), "%s", kWeatherId);
        diagnostics->expectedSchema = 1u;
        diagnostics->expectedSize = 6u * 0x1000u * 2u;
    }
    result = Gen3ResourceSnapshot_FindHandle(snapshot, kWeatherId, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle,
                                          GEN3_RESOURCE_TYPE_BINARY, 1u,
                                          &view);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    if (view.payloadSize != 6u * 0x1000u * 2u)
    {
        if (diagnostics != NULL)
            diagnostics->actualSize = (uint32_t)view.payloadSize;
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    memcpy(sDroughtWeatherColors, view.payload, view.payloadSize);
    status = EMERALD_COMPAT_OK;
    return status;
}

void EmeraldWeatherCompat_ClearMigratedEntries(void)
{
    memset(sDroughtWeatherColors, 0, sizeof(sDroughtWeatherColors));
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */
