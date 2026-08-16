/* R9 §5: Pokémon battle graphics native compatibility publication.
 * See emerald_pokemon_native_compat.h for the contract. */

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
#include "emerald/resources/emerald_pokemon_native_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/pokemon_battle_slots.generated.h"

/* Mirrors the trainer seam's diagnostics helper (this module is a separate
 * translation unit; the trainer seam's copy is static there). */
static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

/* Session-global state: the Pokémon compatibility image is retained for the
 * process session and released only by EmeraldPokemonCompat_Shutdown
 * (mirroring the trainer seam's session image). */
static struct EmeraldResourceCompatibilityImage *sPokemonImage;

/* Resolve one mapping resource through the NORMAL snapshot, verifying type,
 * schema, winner == ROM_BASE and the R9 §5 invariant: the snapshot payload
 * is the pack's DECODED representation of the retail stream (the ROM's
 * LZ77-compressed bytes were decoded at extraction; the three-way equality
 * against the retail ROM pinned those decoded bytes in Stage 3), so its size
 * must equal the mapping's decoded size exactly. The encoded ROM stream is
 * not in the pack and carries no invariant anyway. */
static enum EmeraldResourceCompatStatus
ResolvePokemonResource(const struct Gen3ResourceSnapshot *snapshot,
                       size_t resourceIndex,
                       struct Gen3ResourceView *outView,
                       struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    const struct PokemonBattleCompatResource *r =
        &kPokemonBattleCompatResources[resourceIndex];
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
        snprintf(diagnostics->expectedType, sizeof(diagnostics->expectedType),
                 "%s", Gen3ResourceType_Name(r->type));
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

    /* Resolve enforces type/schema; the winner identity and the decoded
     * payload size are verified here so a non-ROM_BASE or wrong-sized
     * resource fails closed (§11/§12 mirror of the trainer seam). */
    if (outView->winningProviderId == NULL
     || strcmp(outView->winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0
     || outView->payloadSize != r->expectedSize)
    {
        if (diagnostics != NULL)
        {
            snprintf(diagnostics->winningProviderId,
                     sizeof(diagnostics->winningProviderId), "%s",
                     outView->winningProviderId != NULL
                         ? outView->winningProviderId : "?");
            diagnostics->actualSize = (uint32_t)outView->payloadSize;
        }
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }
    return EMERALD_COMPAT_OK;
}

/* Re-encode the pack's DECODED payload into a deterministic literal-only GBA
 * LZ77 stream (Gen3LzLiteral_Encode, Stage R5): the consumers only run the
 * real GBA decompressor over table .data, so the image entries must BE GBA
 * LZ77 streams, and the literal encoder produces a byte-deterministic one
 * that decodes back to exactly the pack's canonical decoded bytes (which are
 * the retail ROM's decode, pinned three-way in Stage 3). All streams for one
 * init go into a single arena, freed after CreateFamily (which copies every
 * payload into its own image allocation). */
static enum EmeraldResourceCompatStatus
EncodePokemonStreams(const struct Gen3ResourceView *views,
                     uint8_t **outArena,
                     struct EmeraldResourceCompatSourceEntry *entries)
{
    size_t total = 0u;
    size_t offset = 0u;
    uint8_t *arena;
    size_t i;

    for (i = 0u; i < POKEMON_BATTLE_RESOURCE_COUNT; i++)
    {
        size_t encoded = Gen3LzLiteral_EncodedSize((uint32_t)views[i].payloadSize);
        if (encoded == SIZE_MAX)
            return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
        total += encoded;
    }
    arena = (uint8_t *)malloc(total != 0u ? total : 1u);
    if (arena == NULL)
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    for (i = 0u; i < POKEMON_BATTLE_RESOURCE_COUNT; i++)
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

/* Validate the image against the generated mapping BEFORE any mutation: entry
 * count must be exactly the mapping's resource count, and each entry's name,
 * type and decoded size must match its mapping row. */
static enum EmeraldResourceCompatStatus
ValidateImageAgainstMapping(
    const struct EmeraldResourceCompatibilityImage *image,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    size_t i;

    if (image == NULL
     || EmeraldResourceCompatImage_GetEntryCount(image)
            != POKEMON_BATTLE_RESOURCE_COUNT)
        return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
    for (i = 0u; i < POKEMON_BATTLE_RESOURCE_COUNT; i++)
    {
        const struct PokemonBattleCompatResource *r =
            &kPokemonBattleCompatResources[i];
        const char *name = EmeraldResourceCompatImage_GetEntryName(image, i);

        if (name == NULL || strcmp(name, r->id) != 0
         || EmeraldResourceCompatImage_GetEntryType(image, i) != r->type
         || EmeraldResourceCompatImage_GetDecodedSize(image, i)
                != r->expectedSize
         || EmeraldResourceCompatImage_GetStream(image, i) == NULL)
        {
            if (diagnostics != NULL)
            {
                snprintf(diagnostics->stage, sizeof(diagnostics->stage),
                         "publish");
                snprintf(diagnostics->canonicalName,
                         sizeof(diagnostics->canonicalName), "%s",
                         r->id);
                diagnostics->expectedSize = r->expectedSize;
            }
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }
    }
    return EMERALD_COMPAT_OK;
}

/* Publish every migrated slot from a validated image. */
static enum EmeraldResourceCompatStatus
PublishPokemonTables(const struct EmeraldResourceCompatibilityImage *image,
                     struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    enum EmeraldResourceCompatStatus status;
    size_t slotIndex = 0u;
    size_t kind;
    size_t idx;

    ClearDiagnostics(diagnostics);
    status = ValidateImageAgainstMapping(image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        return status;

    /* §5: change ONLY the migrated payload pointers. Slots are kind-major in
     * the generated map (front 0..439, back 440..879, normal palette
     * 880..1319, shiny palette 1320..1759, slot index == species id); each
     * publishes the stream of its mapping resource, so multi-slot rows
     * (Unown forms, shared palettes) and cross-family rows (the shiny EGG
     * slot reuses the normal-palette stream) alias naturally. The one
     * external slot (back EGG, gMonStillFrontPic_Egg) is skipped - it has no
     * canonical and stays compiled. Indices, tags, sizes, everything else
     * unchanged. The streams are immutable after construction. */
    for (kind = 0u; kind < 2u; kind++)
    {
        struct CompressedSpriteSheet *table =
            kind == 0u ? gMonFrontPicTable : gMonBackPicTable;
        for (idx = 0u; idx < POKEMON_BATTLE_SLOTS_PER_TABLE; idx++)
        {
            int32_t ri = kPokemonBattleCompatSlots[slotIndex++];
            if (ri != POKEMON_BATTLE_EXTERNAL_SLOT)
                table[idx].data = (const u32 *)(const void *)
                    EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
        }
    }
    for (kind = 0u; kind < 2u; kind++)
    {
        struct CompressedSpritePalette *table =
            kind == 0u ? gMonPaletteTable : gMonShinyPaletteTable;
        for (idx = 0u; idx < POKEMON_BATTLE_SLOTS_PER_TABLE; idx++)
        {
            int32_t ri = kPokemonBattleCompatSlots[slotIndex++];
            if (ri != POKEMON_BATTLE_EXTERNAL_SLOT)
                table[idx].data = (const u32 *)(const void *)
                    EmeraldResourceCompatImage_GetStream(image, (size_t)ri);
        }
    }
    return EMERALD_COMPAT_OK;
}

enum EmeraldResourceCompatStatus
EmeraldPokemonCompat_TryInitialize(
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
        POKEMON_BATTLE_RESOURCE_COUNT, sizeof(entries[0]));
    views = (struct Gen3ResourceView *)calloc(
        POKEMON_BATTLE_RESOURCE_COUNT, sizeof(views[0]));
    if (entries == NULL || views == NULL)
    {
        free(entries);
        free(views);
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }

    /* Phase 1: resolve and verify every resource through the NORMAL snapshot
     * before anything is mutated. Any failure leaves the live tables
     * untouched (still holding their compiled payloads until R9 §7). */
    for (i = 0u; i < POKEMON_BATTLE_RESOURCE_COUNT; i++)
    {
        status = ResolvePokemonResource(snapshot, i, &views[i], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            goto done;
    }

    /* Phase 2: transactional image build. The pack serves the DECODED
     * representation of every retail stream, so the seam re-encodes each
     * payload into a deterministic literal-only GBA LZ77 stream
     * (EMERALD_COMPAT_ENTRY_GBA_LZ): the image entries ARE GBA LZ77 streams
     * whose declared decoded size equals the mapping's decoded size (already
     * verified byte-count-equal in Phase 1), and the streams decode to
     * exactly the pack's canonical decoded bytes. */
    status = EncodePokemonStreams(views, &encodedArena, entries);
    if (status != EMERALD_COMPAT_OK)
        goto done;
    for (i = 0u; i < POKEMON_BATTLE_RESOURCE_COUNT; i++)
    {
        const struct PokemonBattleCompatResource *r =
            &kPokemonBattleCompatResources[i];
        entries[i].canonicalName = r->id;
        entries[i].type = r->type;
        entries[i].schema = r->schema;
        entries[i].expectedSize = r->expectedSize;
        entries[i].encoding = EMERALD_COMPAT_ENTRY_GBA_LZ;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        POKEMON_BATTLE_RESOURCE_COUNT, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        goto done;

    /* Phase 3: publish every slot from the validated image, then adopt it as
     * the session image (replacing any previous one). The encoded arena is
     * transient: CreateFamily copied every stream into the image's own
     * permanent allocation. */
    status = PublishPokemonTables(image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
    {
        EmeraldResourceCompatImage_Destroy(image);
        goto done;
    }
    EmeraldResourceCompatImage_Destroy(sPokemonImage);
    sPokemonImage = image;
    status = EMERALD_COMPAT_OK;
done:
    free(encodedArena);
    free(entries);
    free(views);
    return status;
}

enum EmeraldResourceCompatStatus
EmeraldPokemonCompat_Republish(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    ClearDiagnostics(diagnostics);
    if (sPokemonImage == NULL)
    {
        /* No valid session image: fail closed. Nothing is re-resolved from
         * ROM, no pack is reread, no stream is rebuilt - the only source is
         * the already-valid current-session image, which is absent. */
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "republish");
        return EMERALD_COMPAT_ERR_UNAVAILABLE;
    }
    /* Idempotent, allocation-free: re-derives every pointer from the retained
     * image and rewrites the same migrated slots. */
    return PublishPokemonTables(sPokemonImage, diagnostics);
}

void EmeraldPokemonCompat_ClearMigratedEntries(void)
{
    size_t slotIndex = 0u;
    size_t kind;
    size_t idx;

    /* Fail-closed sentinel: clear only the migrated native table slots. The
     * external back-EGG slot is never touched (it stays compiled). */
    for (kind = 0u; kind < 2u; kind++)
    {
        struct CompressedSpriteSheet *table =
            kind == 0u ? gMonFrontPicTable : gMonBackPicTable;
        for (idx = 0u; idx < POKEMON_BATTLE_SLOTS_PER_TABLE; idx++)
        {
            if (kPokemonBattleCompatSlots[slotIndex++] != POKEMON_BATTLE_EXTERNAL_SLOT)
                table[idx].data = NULL;
        }
    }
    for (kind = 0u; kind < 2u; kind++)
    {
        struct CompressedSpritePalette *table =
            kind == 0u ? gMonPaletteTable : gMonShinyPaletteTable;
        for (idx = 0u; idx < POKEMON_BATTLE_SLOTS_PER_TABLE; idx++)
        {
            if (kPokemonBattleCompatSlots[slotIndex++] != POKEMON_BATTLE_EXTERNAL_SLOT)
                table[idx].data = NULL;
        }
    }
}

void EmeraldPokemonCompat_Shutdown(void)
{
    /* Mirrors the trainer seam: the session image is released at process
     * teardown; no consumer runs afterwards, so the published pointers are
     * left in place. */
    EmeraldResourceCompatImage_Destroy(sPokemonImage);
    sPokemonImage = NULL;
}

#endif /* defined(PLATFORM_SDL2) && defined(NATIVE_LINUX) */
