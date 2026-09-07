/* R15 Phase 4: battle-animation graphics native compatibility publication.
 * Graphics only - the H VM bytecode family is unchanged. See
 * emerald_battle_anim_gfx_compat.h for the contract. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "battle_anim.h"
#include "data.h"

#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_lz.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_battle_anim_gfx_compat.h"
#include "emerald/resources/battle_anim_gfx_slots.generated.h"

/* The two consumer tables that live in other translation units; non-static
 * on native (R15 Phase 4) so this seam can publish into them. */
struct BattleBackground
{
    const void *tileset;
    const void *tilemap;
    const void *entryTileset;
    const void *entryTilemap;
    const void *palette;
};
extern struct BattleAnimBackground gBattleAnimBackgroundTable[];
extern struct CompressedSpriteSheet sBallParticleSpriteSheets[POKEBALL_COUNT];
extern struct BattleBackground sBattleEnvironmentTable[];

static struct EmeraldResourceCompatibilityImage *sImage;

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static enum EmeraldResourceCompatStatus
ResolveGfxResource(const struct Gen3ResourceSnapshot *snapshot,
                   size_t resourceIndex,
                   struct Gen3ResourceView *outView,
                   struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    const struct BattleAnimGfxResource *r =
        &kBattleAnimGfxResources[resourceIndex];
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diagnostics != NULL)
    {
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", r->id);
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "resolve");
        diagnostics->expectedSchema = r->schema;
        diagnostics->expectedSize = r->expectedSize;
    }

    result = Gen3ResourceSnapshot_FindHandle(snapshot, r->id, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    result = Gen3ResourceSnapshot_Resolve(snapshot, handle, r->type,
                                          r->schema, outView);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    if (outView->winningProviderId == NULL
     || strcmp(outView->winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0
     || outView->payloadSize != r->expectedSize)
    {
        if (diagnostics != NULL)
            diagnostics->actualSize = (uint32_t)outView->payloadSize;
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    return EMERALD_COMPAT_OK;
}

static enum EmeraldResourceCompatStatus
EncodeGfxStreams(const struct Gen3ResourceView *views,
                 uint8_t **outArena,
                 struct EmeraldResourceCompatSourceEntry *entries)
{
    size_t total = 0u;
    size_t offset = 0u;
    uint8_t *arena;
    size_t i;

    for (i = 0u; i < BATTLE_ANIM_GFX_RESOURCE_COUNT; i++)
    {
        if (kBattleAnimGfxResources[i].role == EMERALD_RESOURCE_ROLE_CANONICAL)
        {
            entries[i].payload = views[i].payload;
            entries[i].payloadSize = (uint32_t)views[i].payloadSize;
            continue;
        }
        {
            size_t encoded =
                Gen3LzLiteral_EncodedSize((uint32_t)views[i].payloadSize);
            if (encoded == SIZE_MAX)
                return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
            total += encoded;
        }
    }
    arena = (uint8_t *)malloc(total != 0u ? total : 1u);
    if (arena == NULL)
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    for (i = 0u; i < BATTLE_ANIM_GFX_RESOURCE_COUNT; i++)
    {
        size_t encoded;
        enum Gen3LzResult lz;

        if (kBattleAnimGfxResources[i].role == EMERALD_RESOURCE_ROLE_CANONICAL)
            continue;
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

/* Publish every migrated slot from a validated image. Slot sections are
 * kind-major (generated): pic 0..288, pal 289..577, bg 578..658 (3 per row),
 * ball 659..670, env 671..715 (5 per row). */
static enum EmeraldResourceCompatStatus
PublishGfxTables(const struct EmeraldResourceCompatibilityImage *image,
                 struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    size_t slot = 0u;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (image == NULL
     || EmeraldResourceCompatImage_GetEntryCount(image)
            != BATTLE_ANIM_GFX_RESOURCE_COUNT)
        return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
    for (i = 0u; i < BATTLE_ANIM_GFX_PIC_ROWS; i++)
    {
        int32_t ri = kBattleAnimGfxSlots[slot++];
        gBattleAnimPicTable[i].data = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
    }
    for (i = 0u; i < BATTLE_ANIM_GFX_PAL_ROWS; i++)
    {
        int32_t ri = kBattleAnimGfxSlots[slot++];
        gBattleAnimPaletteTable[i].data = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
    }
    for (i = 0u; i < BATTLE_ANIM_GFX_BG_ROWS; i++)
    {
        int32_t riImg = kBattleAnimGfxSlots[slot++];
        int32_t riPal = kBattleAnimGfxSlots[slot++];
        int32_t riTm = kBattleAnimGfxSlots[slot++];
        gBattleAnimBackgroundTable[i].image = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riImg);
        gBattleAnimBackgroundTable[i].palette = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riPal);
        gBattleAnimBackgroundTable[i].tilemap = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riTm);
    }
    for (i = 0u; i < BATTLE_ANIM_GFX_BALL_ROWS; i++)
    {
        int32_t ri = kBattleAnimGfxSlots[slot++];
        sBallParticleSpriteSheets[i].data = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
    }
    for (i = 0u; i < BATTLE_ANIM_GFX_ENV_ROWS; i++)
    {
        int32_t riTiles = kBattleAnimGfxSlots[slot++];
        int32_t riTm = kBattleAnimGfxSlots[slot++];
        int32_t riEntryTiles = kBattleAnimGfxSlots[slot++];
        int32_t riEntryTm = kBattleAnimGfxSlots[slot++];
        int32_t riPal = kBattleAnimGfxSlots[slot++];
        sBattleEnvironmentTable[i].tileset = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riTiles);
        sBattleEnvironmentTable[i].tilemap = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riTm);
        sBattleEnvironmentTable[i].entryTileset = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riEntryTiles);
        sBattleEnvironmentTable[i].entryTilemap = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riEntryTm);
        sBattleEnvironmentTable[i].palette = (const u32 *)(const void *)
            EmeraldResourceCompatImage_GetStream(image, (size_t)riPal);
    }
    return EMERALD_COMPAT_OK;
}

/* Binary-search the generated resource table for an id; the accessor macro
 * header redirects every legacy symbol name here on native. */
static int32_t FindResourceIndex(const char *id)
{
    size_t low = 0u;
    size_t high = BATTLE_ANIM_GFX_RESOURCE_COUNT;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        int cmp = strcmp(kBattleAnimGfxResources[mid].id, id);
        if (cmp == 0)
            return (int32_t)mid;
        if (cmp < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return -1;
}

const void *BattleAnimGfx_Get(const char *id)
{
    int32_t ri;
    if (id == NULL || sImage == NULL)
        return NULL;
    ri = FindResourceIndex(id);
    if (ri < 0)
        return NULL;
    return EmeraldResourceCompatImage_GetStream(sImage, (size_t)ri);
}

enum EmeraldResourceCompatStatus
EmeraldBattleAnimGfxCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    struct EmeraldResourceCompatSourceEntry *entries;
    struct Gen3ResourceView *views;
    struct EmeraldResourceCompatibilityImage *image = NULL;
    uint8_t *encodedArena = NULL;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    entries = (struct EmeraldResourceCompatSourceEntry *)calloc(
        BATTLE_ANIM_GFX_RESOURCE_COUNT, sizeof(entries[0]));
    views = (struct Gen3ResourceView *)calloc(
        BATTLE_ANIM_GFX_RESOURCE_COUNT, sizeof(views[0]));
    if (entries == NULL || views == NULL)
    {
        free(entries);
        free(views);
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }

    for (i = 0u; i < BATTLE_ANIM_GFX_RESOURCE_COUNT; i++)
    {
        status = ResolveGfxResource(snapshot, i, &views[i], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }
    status = EncodeGfxStreams(views, &encodedArena, entries);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    for (i = 0u; i < BATTLE_ANIM_GFX_RESOURCE_COUNT; i++)
    {
        const struct BattleAnimGfxResource *r =
            &kBattleAnimGfxResources[i];
        entries[i].canonicalName = r->id;
        entries[i].type = r->type;
        entries[i].schema = r->schema;
        entries[i].expectedSize = r->expectedSize;
        entries[i].encoding =
            r->role == EMERALD_RESOURCE_ROLE_CANONICAL
                ? EMERALD_COMPAT_ENTRY_RAW
                : EMERALD_COMPAT_ENTRY_GBA_LZ;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        BATTLE_ANIM_GFX_RESOURCE_COUNT, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    status = PublishGfxTables(image, diagnostics);
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

enum EmeraldResourceCompatStatus
EmeraldBattleAnimGfxCompat_Republish(
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    ClearDiagnostics(diagnostics);
    if (sImage == NULL)
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "republish");
        return EMERALD_COMPAT_ERR_UNAVAILABLE;
    }
    return PublishGfxTables(sImage, diagnostics);
}

void EmeraldBattleAnimGfxCompat_ClearMigratedEntries(void)
{
    size_t i;
    for (i = 0u; i < BATTLE_ANIM_GFX_PIC_ROWS; i++)
        gBattleAnimPicTable[i].data = NULL;
    for (i = 0u; i < BATTLE_ANIM_GFX_PAL_ROWS; i++)
        gBattleAnimPaletteTable[i].data = NULL;
    for (i = 0u; i < BATTLE_ANIM_GFX_BG_ROWS; i++)
    {
        gBattleAnimBackgroundTable[i].image = NULL;
        gBattleAnimBackgroundTable[i].palette = NULL;
        gBattleAnimBackgroundTable[i].tilemap = NULL;
    }
    for (i = 0u; i < BATTLE_ANIM_GFX_BALL_ROWS; i++)
        sBallParticleSpriteSheets[i].data = NULL;
    for (i = 0u; i < BATTLE_ANIM_GFX_ENV_ROWS; i++)
    {
        sBattleEnvironmentTable[i].tileset = NULL;
        sBattleEnvironmentTable[i].tilemap = NULL;
        sBattleEnvironmentTable[i].entryTileset = NULL;
        sBattleEnvironmentTable[i].entryTilemap = NULL;
        sBattleEnvironmentTable[i].palette = NULL;
    }
}

void EmeraldBattleAnimGfxCompat_Shutdown(void)
{
    EmeraldResourceCompatImage_Destroy(sImage);
    sImage = NULL;
}

const struct EmeraldResourceCompatibilityImage *EmeraldBattleAnimGfxCompat_GetImage(void)
{
    return sImage;
}

uint32_t EmeraldBattleAnimGfxCompat_GetEntrySchema(size_t entryIndex)
{
    if (entryIndex >= BATTLE_ANIM_GFX_RESOURCE_COUNT)
        return 0u;
    return kBattleAnimGfxResources[entryIndex].schema;
}

uint32_t EmeraldBattleAnimGfxCompat_GetEntryRole(size_t entryIndex)
{
    if (entryIndex >= BATTLE_ANIM_GFX_RESOURCE_COUNT)
        return 0u;
    return kBattleAnimGfxResources[entryIndex].role;
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */
