/* R15 Phase 6: wallpaper graphics native compatibility publication.
 * See emerald_wallpaper_compat.h for the contract. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "data.h"

#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_lz.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_wallpaper_compat.h"
#include "emerald/resources/wallpaper_slots.generated.h"

/* struct Wallpaper is local to pokemon_storage_system.c; replicate here. */
struct Wallpaper
{
    const u32 *tiles;
    const u32 *tilemap;
    const u16 *palettes;
};

/* Tables are non-static on native (declared in wallpapers.h). */
extern struct Wallpaper sWallpapers[];
extern struct Wallpaper sWaldaWallpapers[];
extern const u32 *sWaldaWallpaperIcons[];
/* sArrow_Gfx is pack-served (emerald:wallpaper/misc/arrow); its legacy
 * definition is gated out of the native link. */

static struct EmeraldResourceCompatibilityImage *sImage;

static int32_t FindResourceIndex(const char *id)
{
    size_t low = 0u;
    size_t high = WALLPAPER_RESOURCE_COUNT;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        int cmp = strcmp(kWallpaperResources[mid].id, id);
        if (cmp == 0)
            return (int32_t)mid;
        if (cmp < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return -1;
}

const void *Wallpaper_Get(const char *id)
{
    int32_t ri;
    if (id == NULL || sImage == NULL)
        return NULL;
    ri = FindResourceIndex(id);
    if (ri < 0)
        return NULL;
    return EmeraldResourceCompatImage_GetStream(sImage, (size_t)ri);
}

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diag)
{
    if (diag != NULL)
        memset(diag, 0, sizeof(*diag));
}

static enum EmeraldResourceCompatStatus
ResolveResource(const struct Gen3ResourceSnapshot *snapshot,
                size_t resourceIndex,
                struct Gen3ResourceView *outView,
                struct EmeraldResourceCompatDiagnostics *diag)
{
    const struct WallpaperResource *r = &kWallpaperResources[resourceIndex];
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnostics(diag);
    if (snapshot == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diag != NULL)
    {
        snprintf(diag->canonicalName, sizeof(diag->canonicalName),
                 "%s", r->id);
        diag->expectedSchema = r->schema;
        diag->expectedSize = r->expectedSize;
    }
    result = Gen3ResourceSnapshot_FindHandle(snapshot, r->id, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle, r->type,
                                          r->schema, outView);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    if (outView->payloadSize != r->expectedSize)
    {
        if (diag != NULL)
            diag->actualSize = (uint32_t)outView->payloadSize;
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    return EMERALD_COMPAT_OK;
}

static enum EmeraldResourceCompatStatus
EncodeStreams(const struct Gen3ResourceView *views,
              uint8_t **outArena,
              struct EmeraldResourceCompatSourceEntry *entries)
{
    size_t total = 0u;
    size_t offset = 0u;
    uint8_t *arena;
    size_t i;

    for (i = 0u; i < WALLPAPER_RESOURCE_COUNT; i++)
    {
        size_t encoded =
            Gen3LzLiteral_EncodedSize((uint32_t)views[i].payloadSize);
        if (encoded == SIZE_MAX)
            return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
        total += encoded;
    }
    arena = (uint8_t *)malloc(total != 0u ? total : 1u);
    if (arena == NULL)
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    for (i = 0u; i < WALLPAPER_RESOURCE_COUNT; i++)
    {
        size_t encoded;
        enum Gen3LzResult lz;
        lz = Gen3LzLiteral_Encode(views[i].payload, (uint32_t)views[i].payloadSize,
                                  arena + offset, total - offset, &encoded);
        if (lz != GEN3_LZ_OK)
        {
            free(arena);
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }
        entries[i].payload = arena + offset;
        entries[i].payloadSize = (uint32_t)encoded;
        offset += encoded;
    }
    *outArena = arena;
    return EMERALD_COMPAT_OK;
}

/* Find resource index for a wallpaper component by symbolic name pattern.
 * The slot map is ordered: tiles, tilemaps, frame-pals, bg-pals, icons, misc. */
static int32_t FindWallpaperComponent(const char *wallpaper, const char *component)
{
    size_t i;
    for (i = 0u; i < WALLPAPER_RESOURCE_COUNT; i++)
    {
        const char *id = kWallpaperResources[i].id;
        /* Match: emerald:wallpaper/<name>/<component> */
        if (strstr(id, wallpaper) && strstr(id, component))
        {
            /* Verify it's actually the right path depth */
            /* e.g. "emerald:wallpaper/forest/tiles" matches forest + tiles */
            const char *slash = strrchr(id, '/');
            if (slash && strcmp(slash + 1, component) == 0)
                return (int32_t)i;
        }
    }
    return -1;
}

/* Publish the wallpaper tables from the validated image. */
static enum EmeraldResourceCompatStatus
PublishWallpaperTables(const struct EmeraldResourceCompatibilityImage *image,
                       struct EmeraldResourceCompatDiagnostics *diag)
{
    static const char *defaultWallpapers[] = {
        "forest", "city", "desert", "savanna", "crag", "volcano",
        "snow", "cave", "beach", "seafloor", "river", "sky",
        "polkadot", "pokecenter", "machine", "plain"
    };
    static const char *waldaWallpapers[] = {
        "zigzagoon", "screen", "horizontal", "diagonal", "block",
        "ribbon", "pokecenter2", "frame", "blank", "circles",
        "azumarill", "pikachu", "legendary", "dusclops", "ludicolo", "whiscash"
    };
    size_t i;

    ClearDiagnostics(diag);
    if (image == NULL
     || EmeraldResourceCompatImage_GetEntryCount(image)
            != WALLPAPER_RESOURCE_COUNT)
        return EMERALD_COMPAT_ERR_PUBLISH_FAILED;

    /* Default wallpapers */
    for (i = 0u; i < 16u; i++)
    {
        int32_t ri;
        ri = FindWallpaperComponent(defaultWallpapers[i], "tiles");
        if (ri >= 0)
            sWallpapers[i].tiles = (const u32 *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
        ri = FindWallpaperComponent(defaultWallpapers[i], "tilemap");
        if (ri >= 0)
            sWallpapers[i].tilemap = (const u32 *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
        ri = FindWallpaperComponent(defaultWallpapers[i], "frame-pal");
        if (ri >= 0)
            sWallpapers[i].palettes = (const u16 *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
    }

    /* Walda wallpapers */
    for (i = 0u; i < 16u; i++)
    {
        int32_t ri;
        ri = FindWallpaperComponent(waldaWallpapers[i], "tiles");
        if (ri >= 0)
            sWaldaWallpapers[i].tiles = (const u32 *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
        ri = FindWallpaperComponent(waldaWallpapers[i], "tilemap");
        if (ri >= 0)
            sWaldaWallpapers[i].tilemap = (const u32 *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
        ri = FindWallpaperComponent(waldaWallpapers[i], "frame-pal");
        if (ri >= 0)
            sWaldaWallpapers[i].palettes = (const u16 *)
                EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
    }

    /* Walda wallpaper icons */
    {
        static const char *iconNames[] = {
            "aqua", "heart", "five_star", "brick", "four_star", "asterisk",
            "dot", "cross", "line_circle", "pokeball", "maze", "footprint",
            "big_asterisk", "circle", "koffing", "ribbon", "bolt",
            "four_circles", "lotad", "crystal", "pichu", "diglett",
            "luvdisc", "star_in_circle", "spinda", "latis", "plusle",
            "minun", "togepi", "magma"
        };
        for (i = 0u; i < 30u; i++)
        {
            size_t j;
            for (j = 0u; j < WALLPAPER_RESOURCE_COUNT; j++)
            {
                if (strstr(kWallpaperResources[j].id, iconNames[i])
                 && strstr(kWallpaperResources[j].id, "wallpaper-icon"))
                {
                    sWaldaWallpaperIcons[i] = (const u32 *)
                        EmeraldResourceCompatImage_GetStream(image, j);
                    break;
                }
            }
        }
    }

    return EMERALD_COMPAT_OK;
}

enum EmeraldResourceCompatStatus
EmeraldWallpaperCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diag)
{
    struct EmeraldResourceCompatSourceEntry *entries;
    struct Gen3ResourceView *views;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    uint8_t *encodedArena = NULL;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diag);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    entries = (struct EmeraldResourceCompatSourceEntry *)calloc(
        WALLPAPER_RESOURCE_COUNT, sizeof(entries[0]));
    views = (struct Gen3ResourceView *)calloc(
        WALLPAPER_RESOURCE_COUNT, sizeof(views[0]));
    if (entries == NULL || views == NULL)
    {
        free(entries);
        free(views);
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }

    for (i = 0u; i < WALLPAPER_RESOURCE_COUNT; i++)
    {
        status = ResolveResource(snapshot, i, &views[i], diag);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }
    status = EncodeStreams(views, &encodedArena, entries);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    for (i = 0u; i < WALLPAPER_RESOURCE_COUNT; i++)
    {
        const struct WallpaperResource *r = &kWallpaperResources[i];
        entries[i].canonicalName = r->id;
        entries[i].type = r->type;
        entries[i].schema = r->schema;
        entries[i].expectedSize = r->expectedSize;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_GBA_LZ;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        WALLPAPER_RESOURCE_COUNT, &image, diag);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    status = PublishWallpaperTables(image, diag);
    if (status != EMERALD_COMPAT_OK)
    {
        EmeraldResourceCompatImage_Destroy(image);
        goto done;
    }
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = image;
    status = EMERALD_COMPAT_OK;
done:
    free(encodedArena);
    free(entries);
    free(views);
    return status;
}

void EmeraldWallpaperCompat_ClearMigratedEntries(void)
{
    size_t i;
    for (i = 0u; i < 16u; i++)
    {
        sWallpapers[i].tiles = NULL;
        sWallpapers[i].tilemap = NULL;
        sWallpapers[i].palettes = NULL;
    }
    for (i = 0u; i < 16u; i++)
    {
        sWaldaWallpapers[i].tiles = NULL;
        sWaldaWallpapers[i].tilemap = NULL;
        sWaldaWallpapers[i].palettes = NULL;
    }
    for (i = 0u; i < 30u; i++)
        sWaldaWallpaperIcons[i] = NULL;
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */